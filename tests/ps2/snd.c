/* Sound through the BIOS: rom0:OSDSND is rspu2_driver (the browser's remote
   libspu2, RPC 0x80000601; ps2sdk iop/sound/rspu2drv has its source). It is
   loaded, SpuInit and the master volumes set, and a 375 Hz tone (128 samples
   at 48 kHz) played for 5 seconds (or the seconds of the first argument):
   listen for it. The second argument picks how:
     loop     (default) AutoDMA from a buffer in the IOP heap, looping;
     oneshot  AutoDMA, the buffer once (oneshot() below);
     voice    voice 0 looping ADPCM in SPU RAM (voice() below).
   (-DSND_ONESHOT / -DSND_VOICE pick them for runners without arguments.)

   The calls take their arguments from word 1 on (word 0 unused), and answer
   with a word. AutoDMA input is 16-bit stereo, 512 bytes left then 512 bytes
   right. Loop mode goes into core 1 and sets SPDIF_OUT bit 2, and reads the
   buffer (two halves in turn) at twice 48 kHz: PCSX2 plays it an octave high
   (each sample is written twice for it), the PS2 not at all on its analog
   output, likely only on S/PDIF. One-shot goes into core 0 and is heard. In
   loop mode the status (the IOP address the DMA reads) is read meanwhile;
   it has to stay in the buffer and move. */

#include "t.h"
#include <mx/iop.h>
#include <mx/sif.h>

#define SPU_SID 0x80000601u
#define SPU_INIT 0x0001
#define SPU_SET_CORE 0x0002
#define SPU_AUTODMA_WRITE 0x1020
#define SPU_AUTODMA_STOP 0x1021
#define SPU_AUTODMA_STATUS 0x1022
#define SPU_SET_COMMON_ATTR 0x7128
#define SPU_COMMON_MVOLL 1
#define SPU_COMMON_MVOLR 2
#define SPU_COMMON_CDVOLL (1 << 6)
#define SPU_COMMON_CDVOLR (1 << 7)
#define SPU_COMMON_CDMIX (1 << 9)
#define SPU_COMMON_EXTVOLL (1 << 10)
#define SPU_COMMON_EXTVOLR (1 << 11)
#define SPU_COMMON_EXTMIX (1 << 13)
#define SPU_GET_COMMON_ATTR 0x6128
#define SPU_AUTODMA_LOOP 1

#define SIZE (64 * 1024)

static struct mx_sif_client spu;
static u32 arg[16] MX_ALIGNED(64);
static u32 res[16] MX_ALIGNED(64);
static s16 pcm[SIZE / 2] MX_ALIGNED(64);

/* Call fno with up to three word arguments; returns its answer. */
static u32 spu_call(int fno, u32 a, u32 b, u32 c) {
  volatile u32 *w = MX_UNCACHED(arg);
  w[0] = 0;
  w[1] = a;
  w[2] = b;
  w[3] = c;
  volatile u32 *r = MX_UNCACHED(res);
  r[0] = 0xDEADBEEF;
  CHECK(mx_sif_call(&spu, fno, (void *)w, 16, (void *)r, 4) == 0);
  return r[0];
}

static void wait_ms(u32 ms) {
  u32 t0 = mx_count();
  while (mx_count() - t0 < ms * (MX_EE_HZ / 1000))
    ;
}

/* One-shot (core 0, the input ps2sdk's audsrv uses): ONESHOT bytes of the
   tone at 48 kHz (each sample once), played once; the status is polled until
   AutoDMA ends, which takes ONESHOT / 192000 s at 48 kHz 16-bit stereo. */
#define ONESHOT (512 * 1024)

static void oneshot(const s16 *tone) {
  for (int i = 0; i < SIZE / 2; i++)
    pcm[i] = tone[(i & 255) & 127];
  u32 iop = mx_iop_alloc(ONESHOT);
  mx_printf("snd: one-shot IOP buffer %08x, %u bytes\n", iop, ONESHOT);
  CHECK(iop && !(iop & 15));
  if (!iop)
    return;
  for (u32 o = 0; o < ONESHOT; o += SIZE)
    CHECK(mx_iop_write(iop + o, pcm, SIZE) == 0);
  u32 t0 = mx_count();
  u32 r = spu_call(SPU_AUTODMA_WRITE, iop, ONESHOT, 0);
  mx_printf("snd: SpuAutoDMAWrite = %u\n", r);
  CHECK(mx_iop_write(iop, pcm, SIZE) == 0);
  u32 st, last = 0, t1 = t0;
  int i = 0;
  while ((st = spu_call(SPU_AUTODMA_STATUS, 0, 0, 0)) & 0xFFFFFF) {
    if (i++ % 20 == 0)
      mx_printf("snd: status %08x\n", st);
    last = st & 0xFFFFFF;
    t1 = mx_count();
    if (t1 - t0 > 10 * MX_EE_HZ)
      break;
    wait_ms(50);
  }
  u32 ms = (t1 - t0) / (MX_EE_HZ / 1000);
  mx_printf("snd: one-shot ran %u ms, last read %08x (expected %u ms)\n", ms, last,
            ONESHOT / 192);
  CHECK(ms > ONESHOT / 192 * 8 / 10 && ms < ONESHOT / 192 * 12 / 10);
  spu_call(SPU_AUTODMA_STOP, 0, 0, 0);
  CHECK(mx_iop_free(iop) >= 0);
}

/* Voice: the tone as SPU ADPCM (16-byte blocks of 28 samples: shift and
   filter, flags, 4-bit samples, the first in the low nibble; filter 0 only,
   so a sample is nibble << 12 >> shift). 896 samples (7 periods, 32 blocks)
   loop: the first block has loop start (4), the last loop end and repeat
   (3). Uploaded to SPU RAM at VOICE_ADDR (SpuSetTransferStartAddr, SpuWrite
   from an IOP buffer), played on voice 0 of core 0 at pitch 0x1000 (48 kHz)
   for seconds; its envelope (SpuGetVoiceAttr) has to be up meanwhile. */
#define SPU_SET_KEY 0x0005
#define SPU_SET_TRANSFER_MODE 0x000E
#define SPU_SET_TRANSFER_START 0x0010
#define SPU_WRITE 0x0011
#define SPU_GET_VOICE_ATTR 0x6240
#define SPU_SET_VOICE_ATTR 0x7240
#define VOICE_ADDR 0x10000
#define VOICE_BLOCKS 32

static u8 adpcm[VOICE_BLOCKS * 16] MX_ALIGNED(64);

static void encode(const s16 *x, u8 *b) {
  int m = 0;
  for (int i = 0; i < 28; i++)
    m = x[i] < 0 ? (-x[i] > m ? -x[i] : m) : (x[i] > m ? x[i] : m);
  int sh = 12;
  while (sh > 0 && (7 << (12 - sh)) < m)
    sh--;
  int step = 1 << (12 - sh);
  b[0] = (u8)sh;
  b[1] = 0;
  for (int i = 0; i < 28; i++) {
    int q = (x[i] + (x[i] < 0 ? -step / 2 : step / 2)) / step;
    q = q < -8 ? -8 : q > 7 ? 7 : q;
    if (i & 1)
      b[2 + i / 2] |= (u8)((q & 15) << 4);
    else
      b[2 + i / 2] = (u8)(q & 15);
  }
}

static void voice(const s16 *tone, int seconds) {
  static s16 x[28];
  for (int k = 0; k < VOICE_BLOCKS; k++) {
    for (int i = 0; i < 28; i++)
      x[i] = (s16)(tone[(k * 28 + i) & 127] * 7 / 8);
    encode(x, adpcm + k * 16);
  }
  adpcm[1] = 4;
  adpcm[(VOICE_BLOCKS - 1) * 16 + 1] = 3;

  u32 iop = mx_iop_alloc(sizeof adpcm);
  mx_printf("snd: voice IOP buffer %08x, %u bytes\n", iop, (u32)sizeof adpcm);
  CHECK(iop && !(iop & 15));
  if (!iop)
    return;
  CHECK(mx_iop_write(iop, adpcm, sizeof adpcm) == 0);
  spu_call(SPU_SET_CORE, 0, 0, 0);
  spu_call(SPU_SET_TRANSFER_MODE, 0, 0, 0); /* DMA */
  CHECK_EQ(spu_call(SPU_SET_TRANSFER_START, VOICE_ADDR, 0, 0), VOICE_ADDR);
  CHECK_EQ(spu_call(SPU_WRITE, iop, sizeof adpcm, 0), sizeof adpcm);
  wait_ms(10);

  /* SpuVoiceAttr: voice, mask, volume, volmode, volumex, pitch | note << 16,
     sample_note | envx << 16, addr, loop_addr, a/s/r_mode, ar | dr << 16,
     sr | rr << 16, sl | adsr1 << 16, adsr2 */
  volatile u32 *w = MX_UNCACHED(arg);
  for (int i = 0; i < 16; i++)
    w[i] = 0;
  w[0] = 1;
  w[1] = 0x1 | 0x2 | 0x10 | 0x80 | 0x10000 | 0x20000 | 0x40000; /* vol, pitch, addresses, adsr */
  w[2] = 0x3FFF | (0x3FFFu << 16);
  w[5] = 0x1000;
  w[7] = VOICE_ADDR;
  w[8] = VOICE_ADDR;
  w[14] = 0x000Fu << 16; /* adsr1: fastest attack, sustain level 15 */
  w[15] = 0x1FC0;        /* adsr2: sustain held, fastest release */
  volatile u32 *ra = MX_UNCACHED(res);
  CHECK(mx_sif_call(&spu, SPU_SET_VOICE_ATTR, (void *)w, 64, (void *)ra, 4) == 0);
  spu_call(SPU_SET_KEY, 1, 1, 0);

  int up = 0, n = 0;
  for (int i = 0; i < seconds * 4; i++) {
    wait_ms(250);
    for (int k = 0; k < 16; k++)
      w[k] = 0;
    w[2] = 1;
    CHECK(mx_sif_call(&spu, SPU_GET_VOICE_ATTR, (void *)w, 16, (void *)ra, 64) == 0);
    s16 envx = (s16)(ra[6] >> 16);
    if (i < 4)
      mx_printf("snd: voice 0 envx %d pitch %04x addr %05x\n", envx, ra[5] & 0xFFFF, ra[7]);
    up += envx > 0;
    n++;
  }
  mx_printf("snd: voice envelope up %d of %d times\n", up, n);
  CHECK(up == n);
  spu_call(SPU_SET_KEY, 0, 1, 0);
  CHECK(mx_iop_free(iop) >= 0);
}

int main(int argc, char **argv) {
  int seconds = 5;
  char mode = argc > 2 ? argv[2][0] : 'l';
#ifdef SND_ONESHOT
  mode = 'o';
#endif
#ifdef SND_VOICE
  mode = 'v';
#endif
  if (argc > 1) {
    seconds = 0;
    for (const char *c = argv[1]; *c >= '0' && *c <= '9'; c++)
      seconds = seconds * 10 + *c - '0';
  }
  int id = mx_iop_load_module("rom0:OSDSND");
  mx_printf("snd: rom0:OSDSND id %d\n", id);
  CHECK(id >= 0);
  CHECK(mx_sif_bind_tries(&spu, SPU_SID, 100) == 0);
  if (t_fail)
    return t_done("snd");

  spu_call(SPU_INIT, 0, 0, 0);
  /* Both cores (core 0's output is core 1's external input; loop mode's
     AutoDMA goes into core 1): master, CD and external input volumes, the
     inputs mixed in. SpuCommonAttr: mask, mvol, mvolmode, mvolx, cd (volume,
     reverb, mix), ext (volume, reverb, mix); a volume is left | right << 16. */
  volatile u32 *w = MX_UNCACHED(arg);
  volatile u32 *ra = MX_UNCACHED(res);
  for (int core = 0; core < 2; core++) {
    spu_call(SPU_SET_CORE, (u32)core, 0, 0);
    for (int i = 0; i < 16; i++)
      w[i] = 0;
    w[0] = SPU_COMMON_MVOLL | SPU_COMMON_MVOLR | SPU_COMMON_CDVOLL | SPU_COMMON_CDVOLR |
           SPU_COMMON_CDMIX | SPU_COMMON_EXTVOLL | SPU_COMMON_EXTVOLR | SPU_COMMON_EXTMIX;
    w[1] = 0x3FFF | (0x3FFFu << 16);
    w[4] = 0x7FFF | (0x7FFFu << 16);
    w[6] = 1; /* SPU_ON */
    w[7] = 0x7FFF | (0x7FFFu << 16);
    w[9] = 1;
    CHECK(mx_sif_call(&spu, SPU_SET_COMMON_ATTR, (void *)w, 64, (void *)ra, 4) == 0);
    CHECK(mx_sif_call(&spu, SPU_GET_COMMON_ATTR, (void *)w, 16, (void *)ra, 40) == 0);
    mx_printf("snd: core %d mvol %08x mvolx %08x cd %08x mix %u ext %08x mix %u\n", core, ra[1],
              ra[3], ra[4], ra[6], ra[7], ra[9]);
  }

  /* the tone: a rotating vector, renormalised each period */
  float c = 0.99879546f, s = 0.04906767f; /* cos, sin of 2 pi / 128 */
  float x = 1, y = 0;
  s16 tone[128];
  for (int i = 0; i < 128; i++) {
    tone[i] = (s16)(y * 8000);
    float nx = x * c - y * s, ny = x * s + y * c;
    x = nx, y = ny;
  }
  if (mode == 'o' || mode == 'v') {
    if (mode == 'o')
      oneshot(tone);
    else
      voice(tone, seconds);
    return t_done("snd");
  }
  for (int i = 0; i < SIZE / 2; i++)
    pcm[i] = tone[(i & 255) >> 1]; /* each sample twice; the same left and right */

  u32 iop = mx_iop_alloc(SIZE);
  mx_printf("snd: IOP buffer %08x, %u bytes\n", iop, SIZE);
  CHECK(iop && !(iop & 15));
  if (t_fail)
    return t_done("snd");
  CHECK(mx_iop_write(iop, pcm, SIZE) == 0);

  u32 r = spu_call(SPU_AUTODMA_WRITE, iop, SIZE, SPU_AUTODMA_LOOP);
  mx_printf("snd: SpuAutoDMAWrite = %u\n", r);
  /* rspu2_driver clears (and fades) the start of the buffer: put it back */
  CHECK(mx_iop_write(iop, pcm, SIZE) == 0);

  u32 prev = 0, moves = 0, n = 0;
  for (int i = 0; i < seconds * 4; i++) {
    wait_ms(250);
    u32 st = spu_call(SPU_AUTODMA_STATUS, 0, 0, 0);
    u32 a = st & 0xFFFFFF;
    if (i < 8)
      mx_printf("snd: status %08x\n", st);
    CHECK(a >= iop && a <= iop + SIZE);
    moves += a != prev;
    prev = a;
    n++;
  }
  mx_printf("snd: status moved %u of %u times\n", moves, n);
  CHECK(moves >= n - 1);

  /* the rate: the read position polled every 2 ms (well under a half), its
     steps summed over the loop */
  u32 bytes = 0, t0 = mx_count(), t1;
  prev = (spu_call(SPU_AUTODMA_STATUS, 0, 0, 0) & 0xFFFFFF) - iop;
  do {
    wait_ms(2);
    u32 p = (spu_call(SPU_AUTODMA_STATUS, 0, 0, 0) & 0xFFFFFF) - iop;
    bytes += (p - prev) & (SIZE - 1);
    prev = p;
    t1 = mx_count();
  } while (t1 - t0 < MX_EE_HZ / 2);
  u32 rate = (u32)((u64)bytes * MX_EE_HZ / (t1 - t0));
  mx_printf("snd: AutoDMA reads %u bytes/s (48 kHz 16-bit stereo: 192000)\n", rate);

  spu_call(SPU_AUTODMA_STOP, 0, 0, 0);
  mx_printf("snd: stopped, status %08x\n", spu_call(SPU_AUTODMA_STATUS, 0, 0, 0));
  CHECK(mx_iop_free(iop) >= 0);
  return t_done("snd");
}
