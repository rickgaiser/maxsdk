/* Sound through ps2sdk's audsrv.irx (on libsd.irx), carried in libmx as
   data (<name>_irx .. <name>_irx_end, from the Makefile) and loaded from EE
   memory. audsrv's RPC server is 0x870884e: each function takes its
   arguments as words and answers with a word (ps2sdk ee/rpc/audsrv has its
   client, iop/sound/audsrv its source). Its INIT binds back to an EE server
   of the same id, for its callbacks: mx_sif_serve answers it. */

#include <mx/cpu.h>
#include <mx/iop.h>
#include <mx/sif.h>
#include <mx/snd.h>

#define AUDSRV_SID 0x870884eu
#define AUDSRV_INIT 0x0000
#define AUDSRV_SET_FORMAT 0x0003
#define AUDSRV_PLAY_AUDIO 0x0004
#define AUDSRV_STOP_AUDIO 0x0006
#define AUDSRV_SET_VOLUME 0x0007
#define AUDSRV_INIT_ADPCM 0x0016
#define AUDSRV_LOAD_ADPCM 0x0017
#define AUDSRV_PLAY_ADPCM 0x0018
#define AUDSRV_SET_ADPCM_VOL 0x0019
#define AUDSRV_AVAILABLE 0x001a
#define AUDSRV_QUEUED 0x001b
#define AUDSRV_FREE_ADPCM 0x001c
#define AUDSRV_IS_ADPCM_PLAYING 0x001d

/* audsrv's receive buffer is 18000 bytes: a queue call sends the byte count
   and up to this much data */
#define QUEUE_MAX 16368
#define CHUNK 16384

extern const u8 libsd_irx[], libsd_irx_end[], audsrv_irx[], audsrv_irx_end[];

static struct mx_sif_client srv;
static u32 sbuf[(QUEUE_MAX + 16) / 4] MX_ALIGNED(64);
static u32 arg[4] MX_ALIGNED(64);
static u32 res[4] MX_ALIGNED(64);
static u8 bounce[CHUNK] MX_ALIGNED(64);
static volatile u32 callbacks;
static int ready;

static void callback(int fno, void *arg) {
  (void)fno, (void)arg;
  callbacks++;
}

static int call(int fno, u32 a, u32 b, u32 c) {
  volatile u32 *w = MX_UNCACHED(arg);
  volatile u32 *r = MX_UNCACHED(res);
  w[0] = a;
  w[1] = b;
  w[2] = c;
  w[3] = 0;
  if (mx_sif_call(&srv, fno, (void *)w, 16, (void *)r, 4) < 0)
    return -1;
  return (int)r[0];
}

/* Load and start the module from buf to end; < 0 if it does not load or does
   not stay (NO_RESIDENT_END). */
static int load(const u8 *buf, const u8 *end) {
  int r = 0;
  int id = mx_iop_load_module_buffer(buf, (u32)(end - buf), "", 0, &r);
  return id < 0 || (r & 3) == 1 ? -1 : 0;
}

int mx_snd_init(void) {
  if (ready)
    return 0;
  if (!mx_sif_ready() && mx_sif_init() < 0)
    return -1;
  if (mx_sif_serve(AUDSRV_SID, callback, 0) < 0)
    return -1;
  if (mx_sif_bind_tries(&srv, AUDSRV_SID, 2) < 0 || !srv.server) {
    if (load(libsd_irx, libsd_irx_end) < 0 || load(audsrv_irx, audsrv_irx_end) < 0)
      return -1;
    if (mx_sif_bind_tries(&srv, AUDSRV_SID, 100) < 0 || !srv.server)
      return -1;
  }
  if (call(AUDSRV_INIT, 0, 0, 0) != 0 || call(AUDSRV_INIT_ADPCM, 0, 0, 0) != 0)
    return -1;
  ready = 1;
  return 0;
}

/* ---- the PCM stream ---------------------------------------------------------- */

int mx_snd_stream_format(u32 rate, int bits, int channels) {
  return call(AUDSRV_SET_FORMAT, rate, (u32)bits, (u32)channels) == 0 ? 0 : -1;
}

u32 mx_snd_stream_room(void) {
  int n = call(AUDSRV_AVAILABLE, 0, 0, 0);
  return n < 0 ? 0 : (u32)n;
}

u32 mx_snd_stream_queued(void) {
  int n = call(AUDSRV_QUEUED, 0, 0, 0);
  return n < 0 ? 0 : (u32)n;
}

/* Word 0 the byte count, the data after it; audsrv answers with the bytes
   it took (it takes no more than there is room for). */
u32 mx_snd_stream_queue(const void *data, u32 bytes) {
  const u8 *s = data;
  u32 sent = 0;
  while (bytes) {
    u32 n = bytes < QUEUE_MAX ? bytes : QUEUE_MAX;
    sbuf[0] = n;
    u8 *d = (u8 *)&sbuf[1];
    for (u32 i = 0; i < n; i++)
      d[i] = s[i];
    volatile u32 *r = MX_UNCACHED(res);
    if (mx_sif_call(&srv, AUDSRV_PLAY_AUDIO, sbuf, (n + 4 + 15) & ~15u, (void *)r, 4) < 0)
      break;
    int took = (int)r[0];
    if (took <= 0)
      break;
    sent += (u32)took;
    if ((u32)took < n)
      break;
    s += n;
    bytes -= n;
  }
  return sent;
}

void mx_snd_stream_stop(void) { call(AUDSRV_STOP_AUDIO, 0, 0, 0); }

void mx_snd_stream_volume(int volume) { call(AUDSRV_SET_VOLUME, (u32)volume, 0, 0); }

/* ---- ADPCM ------------------------------------------------------------------ */

/* A block: shift (0 .. 12) and filter in byte 0, flags in byte 1, then 28
   4-bit samples, the first in the low nibble. The SPU decodes a sample as
   (nibble << 12 >> shift) + (s1 * f0 + s2 * f1 + 32 >> 6), clamped, with s1,
   s2 the two samples before. The encoder tries each filter with the shift
   its largest residual needs and one finer, against the decoded history, and
   keeps the one with the least squared error. */
static const int filt[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};

static int clamp16(int v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }

static u64 try_block(const s16 *x, int f, int sh, int *h1, int *h2, u8 *b) {
  int s1 = *h1, s2 = *h2, step = 1 << (12 - sh);
  u64 err = 0;
  for (int i = 0; i < 14; i++)
    b[2 + i] = 0;
  for (int i = 0; i < 28; i++) {
    int pred = (s1 * filt[f][0] + s2 * filt[f][1] + 32) >> 6;
    int r = x[i] - pred;
    int q = (r + (r < 0 ? -step / 2 : step / 2)) / step;
    q = q < -8 ? -8 : q > 7 ? 7 : q;
    int d = clamp16(q * step + pred);
    err += (u64)((x[i] - d) * (x[i] - d));
    s2 = s1;
    s1 = d;
    b[2 + i / 2] |= (u8)((q & 15) << (i & 1 ? 4 : 0));
  }
  b[0] = (u8)(f << 4 | sh);
  *h1 = s1;
  *h2 = s2;
  return err;
}

u32 mx_snd_encode(const s16 *pcm, u32 n, u8 *out, int loop) {
  u32 blocks = (n + 27) / 28;
  int h1 = 0, h2 = 0;
  for (u32 k = 0; k < blocks; k++) {
    s16 x[28];
    for (u32 i = 0; i < 28; i++)
      x[i] = k * 28 + i < n ? pcm[k * 28 + i] : 0;
    u64 best = ~0ull;
    int b1 = 0, b2 = 0;
    u8 b[16];
    for (int f = 0; f < 5; f++) {
      /* the shift for the largest residual, predicted from the source */
      int p1 = h1, p2 = h2, m = 0;
      for (int i = 0; i < 28; i++) {
        int r = x[i] - ((p1 * filt[f][0] + p2 * filt[f][1] + 32) >> 6);
        r = r < 0 ? -r : r;
        m = r > m ? r : m;
        p2 = p1;
        p1 = x[i];
      }
      int sh = 12;
      while (sh > 0 && (7 << (12 - sh)) < m)
        sh--;
      for (int s = sh; s >= 0 && s >= sh - 1; s--) {
        int t1 = h1, t2 = h2;
        u64 e = try_block(x, f, s, &t1, &t2, b);
        if (e < best) {
          best = e;
          b1 = t1;
          b2 = t2;
          for (int i = 0; i < 16; i++)
            out[k * 16 + i] = b[i];
        }
      }
    }
    h1 = b1;
    h2 = b2;
    out[k * 16 + 1] = 0;
  }
  if (loop) {
    out[1] |= 4;                     /* loop start */
    out[(blocks - 1) * 16 + 1] |= 3; /* loop end, repeat */
  } else {
    out[(blocks - 1) * 16 + 1] |= 1; /* end: the voice is released */
  }
  return blocks * 16;
}


/* ---- samples ----------------------------------------------------------------- */

static u32 pitch(u32 rate) {
  u32 p = (rate * 4096u + MX_SND_RATE / 2) / MX_SND_RATE;
  return p > 0x3FFF ? 0x3FFF : p;
}

/* audsrv takes a 16-byte header (word 1: channels << 8, loop << 16; word 2:
   the pitch) and the ADPCM after it, in IOP memory; it copies the ADPCM to
   SPU RAM (libsd's voice transfer, waited for) and answers with 0. The
   copy here is padded to 64 bytes (the SPU2 DMA's block) with silence. */
int mx_snd_load(struct mx_snd_sample *s, const void *adpcm, u32 size, u32 rate) {
  if (!ready)
    return -1;
  u32 total = 16 + ((size + 63) & ~63u);
  u32 iop = mx_iop_alloc(total);
  if (!iop)
    return -1;
  const u8 *src = adpcm;
  for (u32 o = 0; o < total; o += CHUNK) {
    u32 n = total - o < CHUNK ? total - o : CHUNK;
    u8 *b = MX_UNCACHED(bounce);
    for (u32 i = 0; i < n; i++) {
      u32 at = o + i;
      b[i] = at < 16 ? 0 : at - 16 < size ? src[at - 16] : 0;
    }
    if (o == 0) {
      volatile u32 *h = (volatile u32 *)b;
      h[1] = 1u << 8;
      h[2] = pitch(rate);
    }
    if (mx_iop_write(iop + o, b, (n + 15) & ~15u) < 0) {
      mx_iop_free(iop);
      return -1;
    }
  }
  int r = call(AUDSRV_LOAD_ADPCM, iop, total, (u32)(uintptr_t)s);
  mx_iop_free(iop);
  if (r != 0)
    return -1;
  s->rate = rate;
  s->size = size;
  return 0;
}

int mx_snd_free(struct mx_snd_sample *s) {
  return call(AUDSRV_FREE_ADPCM, (u32)(uintptr_t)s, 0, 0) == 0 ? 0 : -1;
}

int mx_snd_play(const struct mx_snd_sample *s, int voice) {
  return call(AUDSRV_PLAY_ADPCM, (u32)voice, (u32)(uintptr_t)s, 0);
}

int mx_snd_playing(int voice, const struct mx_snd_sample *s) {
  return call(AUDSRV_IS_ADPCM_PLAYING, (u32)voice, (u32)(uintptr_t)s, 0) == 1;
}

void mx_snd_voice_volume(int voice, int left, int right) {
  call(AUDSRV_SET_ADPCM_VOL, (u32)voice, (u32)left, (u32)right);
}
