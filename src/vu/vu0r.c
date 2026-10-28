/* The EE side of the VU0 renderer (<mx/vu0r.h>, src/vu/vu0scei.vsm). Built
   with -mvu0 and every VF register fixed: the programs' inputs and results
   stay in VF registers between the asm statements. */

#include <mx/frame.h>
#include <mx/vu.h>
#include <mx/vu0r.h>

typedef s32 s32x4 __attribute__((vector_size(16)));
typedef s16 s16x8 __attribute__((vector_size(16)));
typedef s8 s8x16 __attribute__((vector_size(16)));

extern const u8 vu0scei_vu[], vu0scei_vu_end[];

/* the programs (instructions from the load address) */
enum { INIT = 0, SET_A = 24, SET_B = 52 };
static u32 base = ~0u;

int mx_vu0r_load(u32 addr) {
  u32 bytes = (u32)(vu0scei_vu_end - vu0scei_vu);
  /* VU0 micro memory: 512 instructions */
  if (addr + bytes / 8 > 512)
    return -1;
  base = addr;
  return mx_vu_load(0, vu0scei_vu, bytes, addr);
}

/* VCALLMSR at instruction a of the programs (waits for the running one).
   SYNC.P between: a VCALLMSR right after the CTC2 of CMSAR0 starts at the
   previous CMSAR0 on the PS2 (docs/vu0.md). */
#define CALL(a)                                                                                    \
  __asm__ volatile("ctc2 %0, $vi27\n\tsync.p\n\tvcallmsr $vi27" : : "r"(base + (a)) : "memory")
#define WAIT() __asm__ volatile("cfc2.i $8, $vi29" : : : "$8", "memory")
#define LQC2(p, r) __asm__ volatile("lqc2 $" r ", 0(%0)" : : "r"(p) : "memory")
#define QMTC2(v, r) __asm__ volatile("qmtc2 %0, $" r : : "r"(v))
#define SQC2(p, r) __asm__ volatile("sqc2 $" r ", 0(%0)" : : "r"(p) : "memory")

/* s8x4 -> s32x4 (the bytes sign-extended) */
MX_INLINE s32x4 nrm_in(const void *n) {
  s8x16 b = (s8x16)(s32x4){*(const s32 *)n, 0, 0, 0}, z = {0};
  s16x8 h = (s16x8)__builtin_shufflevector(z, b, 0, 16, 1, 17, 2, 18, 3, 19, 4, 20, 5, 21, 6, 22, 7,
                                           23),
        hz = {0};
  return (s32x4)__builtin_shufflevector(hz, h, 0, 8, 1, 9, 2, 10, 3, 11) >> 24;
}
/* s16x2 -> (s, t, 4096, 0) */
MX_INLINE s32x4 uv_in(const void *u) {
  s16x8 h = (s16x8)(s32x4){*(const s32 *)u, 0, 0, 0}, hz = {0};
  s32x4 w = (s32x4)__builtin_shufflevector(hz, h, 0, 8, 1, 9, 2, 10, 3, 11) >> 16;
  return w | (s32x4){0, 0, 4096, 0};
}

#define IN(i, p, n, u)                                                                             \
  do {                                                                                             \
    s32x4 n_ = nrm_in(nrm + 4 * (i)), u_ = uv_in(uv + 4 * (i));                                    \
    LQC2(pos + 16 * (i), p);                                                                       \
    QMTC2(n_, n);                                                                                  \
    QMTC2(u_, u);                                                                                  \
  } while (0)
#define OUT(o, st, c, x)                                                                           \
  do {                                                                                             \
    SQC2(o, st);                                                                                   \
    SQC2((o) + 2, c);                                                                              \
    SQC2((o) + 4, x);                                                                              \
  } while (0)

/* m >= 1 vertices into o (3 quadwords each), the sets by turns */
static void run(u64 *o, const u8 *pos, const u8 *nrm, const u8 *uv, u32 m) {
  IN(0, "vf1", "vf2", "vf3");
  CALL(SET_A);
  u32 i = 1;
  for (; i + 1 < m; i += 2) {
    IN(i, "vf7", "vf8", "vf9");
    CALL(SET_B);
    OUT(o + 6 * (i - 1), "vf4", "vf5", "vf6");
    IN(i + 1, "vf1", "vf2", "vf3");
    CALL(SET_A);
    OUT(o + 6 * i, "vf10", "vf11", "vf12");
  }
  if (i < m) {
    /* i odd: set B, the last */
    IN(i, "vf7", "vf8", "vf9");
    CALL(SET_B);
    OUT(o + 6 * (i - 1), "vf4", "vf5", "vf6");
    WAIT();
    OUT(o + 6 * i, "vf10", "vf11", "vf12");
  } else {
    WAIT();
    OUT(o + 6 * (i - 1), "vf4", "vf5", "vf6");
  }
}

void mx_vu0r_scei(const float (*uni)[4], u32 prim, const void *pos, const void *nrm, const void *uv,
                  u32 n) {
  if (n < 3)
    return;
  mx_vu0_write(MX_VU0R_UNI, uni, MX_VU0R_UNI_QW);
  CALL(INIT);
  /* REGS: ST, RGBAQ, XYZ2 */
  const u64 regs = MX_GS_REG_ST | MX_GS_REG_RGBAQ << 4 | MX_GS_REG_XYZ2 << 8;
  u32 batch = (mx_frame3_room() - 1) / 3;
  if (batch > MX_VU0R_BATCH)
    batch = MX_VU0R_BATCH;
  for (u32 v = 0;; v += batch - 2) {
    u32 m = n - v < batch ? n - v : batch;
    u64 *c = mx_frame3_reserve(1 + 3 * m);
    c = mx_e_giftag(c, m, 1, (s32)prim, MX_GIF_FLG_PACKED, 3, regs);
    run(c, (const u8 *)pos + 16 * v, (const u8 *)nrm + 4 * v, (const u8 *)uv + 4 * v, m);
    mx_frame3_commit(c + 6 * m);
    if (v + m >= n)
      break;
  }
}
