/* HLSL loops on VU0: a run-time loop in a run-time loop (tests/ps2/nest.hlsl),
   for item counts that take each way through the inner, pipelined loop. */
#include "nest.h"
#include "t.h"

extern const u8 nest_vu[], nest_vu_end[];
static float x[64][4] MX_ALIGNED(64), out[64][4] MX_ALIGNED(64);
static u32 hdr[8] MX_ALIGNED(16);

int main(void) {
  struct mx_kernel k = MX_VU_KERNEL(NEST, 0, nest_vu, nest_vu_end);
  mx_vu_reset(1);
  mx_vu_load(0, k.code, k.code_bytes, 0);
  static struct mx_vuq q;
  mx_vuq_init(&q, 0, 1);
  for (u32 passes = 1; passes <= 3; passes++)
    for (u32 n = 1; n <= 20; n++) {
      for (u32 i = 0; i < n; i++)
        for (int f = 0; f < 4; f++)
          x[i][f] = (float)(s32)(i + (u32)f);
      mx_dcache_writeback(x, sizeof x);
      hdr[4 * VU_NEST_PASSES] = passes;
      mx_event e = mx_vuq_batch(&q, &k, hdr, x, n);
      if (mx_vuq_wait(&q, e)) {
        mx_printf("loops: passes %u n %u did not end\n", passes, n);
        t_fail++;
        continue;
      }
      mx_vuq_read(&q, &k, out, n);
      for (u32 i = 0; i < n; i++) {
        float want = (float)(s32)i;
        for (u32 p = 1; p <= passes; p++)
          want = want * 3 + (float)(s32)p;
        if (out[i][0] != want) {
          mx_printf("loops: passes %u n %u item %u: %d, expected %d\n", passes, n, i,
                    (s32)out[i][0], (s32)want);
          t_fail++;
          break;
        }
      }
    }
  return t_done("loops");
}
