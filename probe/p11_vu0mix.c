/* VU0 in macro and micro mode in one program (not at the same time): what
   happens to a macro instruction issued while a micro program runs, and to
   VF registers across a micro program? The micro program: the fBm kernel
   (apps/procgen/fbm.hlsl, 254 points, 6 octaves, ~200 k cycles), started by
   VCALLMS (mx_vuq without DMA). Macro instructions: VADD writing vf25 (the
   kernel uses it), plain and with the interlocked QMFC2.I; the kernel's
   results compared with those of an undisturbed run. */
#include <mx/mx.h>
#include "fbm.h"

extern const u8 fbm_vu[], fbm_vu_end[];

#define N 254
static float in[N][4] MX_ALIGNED(64);
static float ref[N][4] MX_ALIGNED(64);
static float got[N][4] MX_ALIGNED(64);
static u32 hdr[8] MX_ALIGNED(16);
static u32 v20[4] MX_ALIGNED(16);

enum { NONE, VADD, VADD_I };

static u32 run(struct mx_vuq *q, const struct mx_kernel *k, int macro, float (*out)[4],
               u32 *busy_after) {
  mx_event e = mx_vuq_batch(q, k, hdr, in, N);
  u32 t0 = mx_count();
  if (macro == VADD)
    __asm__ volatile("vadd.xyzw $vf25, $vf0, $vf0" ::: "memory");
  else if (macro == VADD_I) {
    u64 r;
    __asm__ volatile("vadd.xyzw $vf25, $vf0, $vf0\n\tqmfc2.i %0, $vf25" : "=r"(r) : : "memory");
  }
  u32 t = mx_count() - t0;
  *busy_after = mx_vpu_stat() & MX_VPU_VBS0;
  mx_vuq_wait(q, e);
  mx_vuq_read(q, k, out, N);
  return t;
}

int main(void) {
  struct mx_kernel k = MX_VU_KERNEL(FBM, 0, fbm_vu, fbm_vu_end);
  mx_vu_reset(1);
  mx_vu_load(0, k.code, k.code_bytes, 0);
  for (int i = 0; i < N; i++)
    in[i][0] = 300.5f + (float)i * 0.37f, in[i][1] = 200.25f + (float)i * 0.11f, in[i][2] = in[i][3] = 0;
  hdr[4 * VU_FBM_OCT] = 6;
  static struct mx_vuq q;
  mx_vuq_init(&q, 0, 0);
  u32 busy;
  u32 tref = run(&q, &k, NONE, ref, &busy);
  mx_printf("vu0mix: no macro instruction: %u cycles to return, VU0 busy after: %s\n", tref,
            busy ? "yes" : "no");
  static const char *const names[] = {"", "VADD vf25 (plain)", "VADD vf25 + QMFC2.I"};
  for (int m = VADD; m <= VADD_I; m++) {
    u32 t = run(&q, &k, m, got, &busy);
    int same = 0;
    for (int i = 0; i < N; i++)
      same += got[i][2] == ref[i][2];
    mx_printf("vu0mix: %s right after VCALLMS: %u cycles, VU0 busy after it: %s, results equal: %d "
              "of %d\n",
              names[m], t, busy ? "yes" : "no", same, N);
  }
  /* a VF register set in macro mode, read back after a micro program */
  for (int i = 0; i < 4; i++)
    v20[i] = 0x3f800000u + (u32)i;
  mx_dcache_writeback(v20, sizeof v20);
  __asm__ volatile("lqc2 $vf20, 0(%0)" : : "r"(v20) : "memory");
  run(&q, &k, NONE, got, &busy);
  u32 back[4] MX_ALIGNED(16);
  __asm__ volatile("sqc2 $vf20, 0(%0)" : : "r"(back) : "memory");
  mx_printf("vu0mix: vf20 set by macro code, after the micro program: %s\n",
            back[0] == v20[0] && back[3] == v20[3] ? "kept" : "changed (the registers are shared)");
  mx_vuq_release(&q);
  MX_TEST_END(0);
  return 0;
}
