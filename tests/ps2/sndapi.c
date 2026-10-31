/* mx/snd.h: sound through audsrv. Listen for, in turn:
     a 375 Hz tone, the PCM stream (48 kHz 16-bit stereo), 3 seconds (or the
       seconds of the argument): the stream has to take 192000 bytes a
       second;
     a 440 Hz tone, a sample, one-shot, 2 seconds: it has to end by itself
       after 2 s;
     a short 880 Hz blip, a sample at a 24 kHz rate, fading out.
   Before that the encoder is checked on the EE: a sine and a sweep, encoded
   and decoded again (as the SPU does), have to keep a signal to noise ratio
   of 25 dB (300), all filters better than filter 0 alone. */

#include "t.h"
#include <mx/snd.h>

#define LONG_N (2 * 48000)
#define BLIP_N (24000 / 5)

static s16 pcm[LONG_N];
static u8 adpcm[MX_SND_ADPCM_SIZE(LONG_N)];
static s16 dec[LONG_N + 28];
/* amp * sin(2 pi freq t / rate) from i = 0, by a rotating vector */
static void sine(s16 *x, u32 n, float freq, float rate, float amp) {
  float w = 6.2831853f * freq / rate, w2 = w * w;
  float c = 1 - w2 / 2 + w2 * w2 / 24, s = w - w * w2 / 6 + w * w2 * w2 / 120;
  float re = 1, im = 0;
  for (u32 i = 0; i < n; i++) {
    x[i] = (s16)(im * amp);
    float nr = re * c - im * s, ni = re * s + im * c;
    re = nr, im = ni;
    if ((i & 1023) == 1023) {
      float m = 1 / __builtin_sqrtf(re * re + im * im);
      re *= m, im *= m;
    }
  }
}

/* the SPU's decoding (docs: mx_snd_encode) */
static void decode(const u8 *b, u32 blocks, s16 *out) {
  static const int f[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};
  int s1 = 0, s2 = 0;
  for (u32 k = 0; k < blocks; k++, b += 16) {
    int sh = b[0] & 15, fi = b[0] >> 4;
    for (int i = 0; i < 28; i++) {
      int nib = (b[2 + i / 2] >> (i & 1 ? 4 : 0)) & 15;
      int d = (s16)(nib << 12) >> sh;
      d += (s1 * f[fi][0] + s2 * f[fi][1] + 32) >> 6;
      d = d < -32768 ? -32768 : d > 32767 ? 32767 : d;
      *out++ = (s16)d;
      s2 = s1;
      s1 = d;
    }
  }
}

static u32 snr(const s16 *x, u32 n) {
  u32 blocks = mx_snd_encode(x, n, adpcm, 0) / 16;
  decode(adpcm, blocks, dec);
  u64 sig = 0, err = 0;
  for (u32 i = 0; i < n; i++) {
    sig += (u64)(x[i] * x[i]);
    err += (u64)((x[i] - dec[i]) * (x[i] - dec[i]));
  }
  return err ? (u32)(sig / err) : ~0u;
}

static u32 filters_used(u32 n) {
  u32 used = 0;
  for (u32 k = 0; k < MX_SND_ADPCM_SIZE(n) / 16; k++)
    used |= 1u << (adpcm[k * 16] >> 4);
  return used;
}

static void wait_ms(u32 ms) {
  u32 t0 = mx_count();
  while (mx_count() - t0 < ms * (MX_EE_HZ / 1000))
    ;
}

int main(int argc, char **argv) {
  int seconds = 3;
  if (argc > 1) {
    seconds = 0;
    for (const char *c = argv[1]; *c >= '0' && *c <= '9'; c++)
      seconds = seconds * 10 + *c - '0';
  }

  /* the encoder */
  sine(pcm, 4800, 440, 48000, 12000);
  u32 r1 = snr(pcm, 4800);
  u32 u1 = filters_used(4800);
  float ph = 0;
  for (u32 i = 0; i < 4800; i++) { /* a sweep: 100 Hz .. 4 kHz */
    float f = 100 + 3900.0f * i / 4800;
    ph += 6.2831853f * f / 48000;
    if (ph > 3.14159265f)
      ph -= 6.2831853f;
    float p2 = ph * ph; /* sin by its series, |ph| <= pi */
    float sn = ph * (1 - p2 / 6 * (1 - p2 / 20 * (1 - p2 / 42 * (1 - p2 / 72 * (1 - p2 / 110)))));
    pcm[i] = (s16)(sn * 12000);
  }
  u32 r2 = snr(pcm, 4800);
  mx_printf("snd: encoder SNR sine %u (filters %02x), sweep %u (filters %02x)\n", r1, u1, r2,
            filters_used(4800));
  CHECK(r1 > 300 && r2 > 300);
  CHECK(u1 & ~1u);

  u32 t0 = mx_count();
  CHECK(mx_snd_init() == 0);
  mx_printf("snd: init %u ms\n", (mx_count() - t0) / (MX_EE_HZ / 1000));
  if (t_fail)
    return t_done("sndapi");

  /* the stream: a period of the tone, 128 frames, queued as there is room */
  static s16 period[128 * 2];
  sine(pcm, 128, 375, 48000, 10000);
  for (int i = 0; i < 128; i++)
    period[2 * i] = period[2 * i + 1] = pcm[i];
  CHECK(mx_snd_stream_format(48000, 16, 2) == 0);
  u32 room = mx_snd_stream_room();
  mx_printf("snd: stream room %u bytes\n", room);
  u32 total = 0, pos = 0, calls = 0;
  t0 = mx_count();
  while (mx_count() - t0 < (u32)seconds * MX_EE_HZ) {
    room = mx_snd_stream_room() & ~3u;
    while (room) {
      u32 n = sizeof period - pos < room ? sizeof period - pos : room;
      u32 took = mx_snd_stream_queue((const u8 *)period + pos, n);
      total += took;
      pos = (pos + took) % sizeof period;
      room -= took;
      calls++;
      if (took < n)
        break;
    }
    wait_ms(10);
  }
  u32 ms = (mx_count() - t0) / (MX_EE_HZ / 1000);
  u32 queued = mx_snd_stream_queued();
  u32 rate = (u32)((u64)(total - queued) * 1000 / ms);
  mx_printf("snd: stream took %u bytes in %u ms (%u calls), %u queued: %u bytes/s (192000)\n",
            total, ms, calls, queued, rate);
  CHECK(rate > 182000 && rate < 202000);
  mx_snd_stream_stop();
  wait_ms(300);

  /* 2 s one-shot sample */
  static struct mx_snd_sample lng, blip;
  sine(pcm, LONG_N, 440, 48000, 10000);
  u32 size = mx_snd_encode(pcm, LONG_N, adpcm, 0);
  t0 = mx_count();
  CHECK(mx_snd_load(&lng, adpcm, size, 48000) == 0);
  mx_printf("snd: sample %u bytes loaded in %u ms\n", size, (mx_count() - t0) / (MX_EE_HZ / 1000));
  t0 = mx_count();
  int v = mx_snd_play(&lng, -1);
  CHECK(v > 0);
  while (mx_snd_playing(v, &lng) && mx_count() - t0 < 4 * MX_EE_HZ)
    wait_ms(10);
  ms = (mx_count() - t0) / (MX_EE_HZ / 1000);
  mx_printf("snd: sample played %u ms (2000) on voice %d\n", ms, v);
  CHECK(ms > 1900 && ms < 2200);

  /* blip at 24 kHz */
  sine(pcm, BLIP_N, 880, 24000, 10000);
  for (u32 i = 0; i < BLIP_N; i++) /* fade out */
    pcm[i] = (s16)(pcm[i] * (int)(BLIP_N - i) / (int)BLIP_N);
  size = mx_snd_encode(pcm, BLIP_N, adpcm, 0);
  CHECK(mx_snd_load(&blip, adpcm, size, 24000) == 0);
  t0 = mx_count();
  v = mx_snd_play(&blip, -1);
  CHECK(v > 0);
  while (mx_snd_playing(v, &blip) && mx_count() - t0 < 2 * MX_EE_HZ)
    wait_ms(5);
  ms = (mx_count() - t0) / (MX_EE_HZ / 1000);
  mx_printf("snd: blip played %u ms (200) on voice %d\n", ms, v);
  CHECK(ms > 150 && ms < 300);

  CHECK(mx_snd_free(&lng) == 0 && mx_snd_free(&blip) == 0);
  return t_done("sndapi");
}
