/* PLAN_PROCGEN P5: are the VU's field copies exact? The VU target copies a
   field with an FMAC instruction (ADD<f>.<f> d, vf0, s for x, y, z;
   MUL<f>.w d, vf0, s for w, as vf0.w is 1). docs/pcsx2.md: the PS2's
   multiplier is 1 ULP low with a power of two in fs. VU0 in macro mode (the
   same FMAC units), on random bit patterns of normal floats, against the
   input: each idiom's count of results that differ. */
#include <mx/mx.h>

typedef float f32x4 __attribute__((vector_size(16)));
static u32 in[4] MX_ALIGNED(16), out[4] MX_ALIGNED(16);

#define COPY(name, insn)                                                                           \
  static void name(void) {                                                                         \
    __asm__ volatile("lqc2 $vf1, 0(%1)\n\tvsub.xyzw $vf2, $vf0, $vf0\n\t" insn                       \
                     "\n\tsqc2 $vf2, 0(%0)"                                                        \
                     :                                                                             \
                     : "r"(out), "r"(in)                                                           \
                     : "memory");                                                                  \
  }

COPY(mul_w_fs, "vmulw.w $vf2, $vf0, $vf1w")      /* 1.0 in fs: d.w = 1 * s.w */
COPY(mul_w_ft, "vmulw.w $vf2, $vf1, $vf0w")      /* 1.0 in ft: d.w = s.w * 1 */
COPY(mul_x_fs, "vmulx.w $vf2, $vf0, $vf1x")      /* d.w = 1 * s.x */
COPY(add_x, "vaddx.x $vf2, $vf0, $vf1x")         /* d.x = 0 + s.x */
COPY(add_z, "vaddz.z $vf2, $vf0, $vf1z")         /* d.z = 0 + s.z */
COPY(move_w, "vmove.w $vf2, $vf1")                /* d.w = s.w */
COPY(mr32_w, "vmr32.w $vf2, $vf1")                /* d.w = s.x */
COPY(acc_w, "vmulaw.w $ACC, $vf1, $vf0w\n\tvmaddx.w $vf2, $vf0, $vf0x") /* via ACC */
COPY(acc_add, "vaddax.w $ACC, $vf1, $vf0x\n\tvmaddx.w $vf2, $vf0, $vf0x") /* ACC = s + 0 */
COPY(acc_msub, "vaddax.w $ACC, $vf1, $vf0x\n\tvmsubx.w $vf2, $vf0, $vf0x") /* ACC - 1 * 0 */
COPY(add_w_fs, "vaddw.w $vf2, $vf1, $vf0x")      /* d.w = s.w + 0 */
COPY(div_x, "vdiv $Q, $vf1x, $vf0w\n\tvwaitq\n\tvaddq.x $vf2, $vf0, $Q") /* s.x / 1 */

static const struct {
  const char *name;
  void (*f)(void);
  int src; /* the field of the input that should come out */
  int dst;
} idioms[] = {
    {"MULw.w d, vf0, s (1.0 in fs; the w insert)", mul_w_fs, 3, 3},
    {"MULw.w d, s, vf0w (1.0 in ft)", mul_w_ft, 3, 3},
    {"MULx.w d, vf0, s (1.0 in fs, from x)", mul_x_fs, 0, 3},
    {"ADDx.x d, vf0, s (the x insert)", add_x, 0, 0},
    {"ADDz.z d, vf0, s", add_z, 2, 2},
    {"MOVE.w d, s", move_w, 3, 3},
    {"MR32.w d, s (from x)", mr32_w, 0, 3},
    {"MULAw.w ACC, s, vf0w; MADDx.w d, vf0, vf0x", acc_w, 3, 3},
    {"ADDAx.w ACC, s, vf0x; MADDx.w d, vf0, vf0x", acc_add, 3, 3},
    {"ADDAx.w ACC, s, vf0x; MSUBx.w d, vf0, vf0x", acc_msub, 3, 3},
    {"ADDx.w d, s, vf0x (s + 0)", add_w_fs, 3, 3},
    {"DIV q, s.x, vf0.w; ADDq.x d, vf0, q", div_x, 0, 0},
};

int main(void) {
  for (int pow2 = 0; pow2 < 2; pow2++) {
  mx_printf("vucopy: %s\n", pow2 ? "powers of two (mantissa 0)" : "random mantissas");
  u32 seed = 11;
  for (u32 k = 0; k < sizeof idioms / sizeof idioms[0]; k++) {
    u32 wrong = 0, low = 0, first = 0, got1 = 0;
    for (int i = 0; i < 4096; i++) {
      for (int f = 0; f < 4; f++) {
        seed = seed * 1103515245u + 12345u;
        /* exponent 100..154 (2^-27..2^27), any mantissa, either sign */
        in[f] = (seed & (pow2 ? 0x80000000u : 0x807FFFFFu)) | (100u + (seed >> 9) % 55) << 23;
      }
      mx_dcache_writeback(in, sizeof in);
      idioms[k].f();
      u32 want = in[idioms[k].src], got = out[idioms[k].dst];
      if (got != want) {
        if (!wrong)
          first = want, got1 = got;
        wrong++;
        low += (got & 0x7FFFFFFFu) == (want & 0x7FFFFFFFu) - 1;
      }
    }
    mx_printf("vucopy: %-46s %4u of 4096 differ (%u by 1 ULP towards 0)", idioms[k].name, wrong,
              low);
    if (wrong)
      mx_printf(", e.g. %08x -> %08x", first, got1);
    mx_printf("\n");
  }
  }
  MX_TEST_END(0);
  return 0;
}
