/* PLAN_PROCGEN P3: the terrain noise (demos/procgen/fbm.hlsl) on VU0 through a VIF0
   queue, for a 33 x 33 chunk, against the same expressions on the EE, for 1,
   2 and 6 octaves. The hash takes the fraction of a product of about 4500,
   where a float step is 2^-11: the VU's order of rounding (ACC chains) moves
   a hash by about 5e-4, so values agree to 3e-3. Each octave doubles the
   coordinates, and with them their rounding: a point next to the edge of a
   cell may fall into the next one, which changes that octave's term, by
   less than 0.1 from the third octave on. So: 90 % of the samples within
   3e-3, 99 % within 0.1. Also the VU0 cycles per sample. */

#include "t.h"
#include "fbm.h"

extern const u8 fbm_vu[], fbm_vu_end[];

#define S 33
#define NS (S * S)
static float in[NS][4] MX_ALIGNED(64);
static float vu[NS][4] MX_ALIGNED(64);
static int OCT = 6;
static u32 hdr[8] MX_ALIGNED(16); /* n, oct */

/* floor, exactly: the VU's floor (LLVM's VU target) is exact on the PS2's VU;
   the magic-constant floor on the FPU is not (the FPU and the VU cut the
   smaller operand of an add to one bit below the sum's last) */
static float floor_(float v) {
  s32 i = (s32)v;
  return (float)i > v ? (float)(i - 1) : (float)i;
}

static float lerp_(float a, float b, float t) { return a + (b - a) * t; }

static float fbm_ee(float x, float y) {
  float sum = 0, amp = 0.5f;
  for (int o = 0; o < OCT; o++) {
    float ix = floor_(x), iy = floor_(y), fx = x - ix, fy = y - iy, h[4];
    for (int k = 0; k < 4; k++) {
      float cx = ix + (float)(k & 1), cy = iy + (float)(k >> 1);
      float a = cx * 0.1031f, b = cy * 0.1031f;
      a = a - floor_(a);
      b = b - floor_(b);
      float d = a * (b + 33.33f) + b * (a + 33.33f) + a * (a + 33.33f);
      float v = (a + d + b + d) * (a + d);
      h[k] = v - floor_(v);
    }
    float sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
    sum = sum + (lerp_(lerp_(h[0], h[1], sx), lerp_(h[2], h[3], sx), sy) * 2 - 1) * amp;
    amp = amp * 0.5f;
    float nx = (x * 0.8f - y * 0.6f) * 2 + 17.1f;
    y = (x * 0.6f + y * 0.8f) * 2 + 3.7f;
    x = nx;
  }
  return sum;
}

int main(void) {
  struct mx_kernel k = MX_VU_KERNEL(FBM, 0, fbm_vu, fbm_vu_end);
  CHECK(k.code_bytes <= 4096);
  mx_vu_reset(1);
  mx_vu_load(0, k.code, k.code_bytes, 0);
  for (int z = 0; z < S; z++)
    for (int x = 0; x < S; x++) {
      float *p = in[z * S + x];
      p[0] = 100.0f + (float)x * 0.125f, p[1] = 50.0f + (float)z * 0.125f, p[2] = p[3] = 0;
    }
  mx_dcache_writeback(in, sizeof in);

  static const int octs[3] = {1, 2, 6};
  for (int t = 0; t < 3; t++) {
  OCT = octs[t];
  hdr[4 * VU_FBM_OCT] = (u32)OCT;
  mx_dcache_writeback(in, sizeof in);
  static struct mx_vuq q;
  mx_vuq_init(&q, 0, 1);
  mx_vuq_readback_spr(&q, (void *)0x70000000, 0x2000);
  u32 per = mx_kernel_max_items(&k), cycles = 0, batches = 0;
  for (u32 at = 0; at < NS; at += per) {
    u32 n = NS - at < per ? NS - at : per;
    u32 t0 = mx_count();
    mx_event e = mx_vuq_batch(&q, &k, hdr, in[at], n);
    if (mx_vuq_wait(&q, e)) {
      mx_printf("noise: batch at %u did not end\n", at);
      t_fail++;
      break;
    }
    cycles += mx_count() - t0;
    batches++;
    mx_vuq_read(&q, &k, vu[at], n);
  }
  mx_vuq_release(&q);

  u32 close = 0, near = 0;
  u32 far = 0;
  float lo = 10, hi = -10;
  for (u32 i = 0; i < NS; i++) {
    float e = fbm_ee(in[i][0], in[i][1]), d = vu[i][2] - e;
    if (d < 0)
      d = -d;
    near += d < 0.1f;
    if (d < 3e-3f)
      close++;
    else if (far++ < 4)
      mx_printf("noise: sample %u: VU0 %d/10000, EE %d/10000\n", i, (s32)(vu[i][2] * 10000),
                (s32)(e * 10000));
    lo = vu[i][2] < lo ? vu[i][2] : lo;
    hi = vu[i][2] > hi ? vu[i][2] : hi;
  }
  mx_printf("noise: %u of %u samples agree, %u within 0.1; heights %d..%d/1000; %u batches of up to %u, "
            "%u cycles per sample (VU0, with DMA in and out)\n",
            close, NS, near, (s32)(lo * 1000), (s32)(hi * 1000), batches, per, cycles / NS);
  mx_printf("noise: %d octaves\n", OCT);
  CHECK(close * 100 >= NS * 90);
  CHECK(near * 100 >= NS * 99);
  CHECK(hi - lo > 0.5f);
  }
  return t_done("noise");
}
