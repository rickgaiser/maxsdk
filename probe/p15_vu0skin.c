/* VU0 skinning three ways: macro mode (demos/model/model.c's loops, the EE
   issuing every COP2 instruction), micro mode fed through VF registers (the
   EE fills one set while VU0 runs the program of the other: QMTC2 in,
   VCALLMS, SQC2 / QMFC2 out; "Efficient CPU and VU0 Usage", SCEE DevCon
   2003) and micro mode fed by VIF0 DMA (batches of 32 vertices unpacked
   from the model's own formats, read back by toSPR). The vertices are the
   model's (RPOS V3-16, RNRM V4-8, JNT slot bytes, WGT u8 weights largest
   first), the kernels probe/vskin0.vsm. EE cycles per vertex, and the
   results against macro mode's. */
#include <mx/mx.h>

typedef float f32x4 __attribute__((vector_size(16)));
typedef s32 s32x4 __attribute__((vector_size(16)));
typedef s16 s16x8 __attribute__((vector_size(16)));
typedef s8 s8x16 __attribute__((vector_size(16)));
typedef u8 u8x16 __attribute__((vector_size(16)));
typedef u64 u64x2 __attribute__((vector_size(16)));
typedef u8 u8x4 __attribute__((vector_size(4)));
typedef s8 s8x4 __attribute__((vector_size(4)));

extern const u8 vskin0_vu[], vskin0_vu_end[];
#define VS0_A 0
#define VS0_B 64
#define VS0_BATCH 128
#define VS0_INIT 192
#define VS0_PA 256
#define VS0_PB 320
#define BATCH 32 /* vertices of a VIF0 batch */
#define PAL_QW 128 /* 4 columns x 32 slots */

#define N 4096
#define JOINTS 24
/* The streams of a vertex start 640 bytes apart in the 4 KB of a D-cache
   way: aligned 16 KB arrays put element i of each in the same set, and four
   streams in two ways miss on every access (189 cycles a vertex). */
static struct {
  s16 pos[3 * N + 8];
  u8 pad1[624];
  s8x4 nrm[N];
  u8 pad2[640];
  u32 jnt[N];
  u8 pad3[640];
  u8x4 wgt[N];
  u8 pad4[640];
  f32x4 op[4][N];
  u8 pad5[640];
  s8x4 on[4][N];
} D MX_ALIGNED(64);
#define pos D.pos
#define nrm D.nrm
#define jnt D.jnt
#define wgt D.wgt
#define op D.op
#define on D.on
static f32x4 pm[JOINTS][4] MX_ALIGNED(64); /* macro mode's: position read as q * 65536 */
static f32x4 vpal[2][PAL_QW] MX_ALIGNED(64); /* by column: for q * 65536, for q (VIF0) */
static s32x4 mtmp[BATCH] MX_ALIGNED(64);
static u64 chains[N / BATCH][12] MX_ALIGNED(64);

static u32 seed = 1;
static u32 rnd(void) { return seed = seed * 1103515245u + 12345u, seed >> 8; }
static float frnd(void) { return (float)(s32)(rnd() & 0xFFFF) / 32768.0f - 1.0f; }

/* ---- macro mode: model.c's loops ----------------------------------------- */

MX_INLINE f32x4 skin_pos_in(const s16 *p) {
  u64 v;
  __builtin_memcpy(&v, p, 8);
  s16x8 h = (s16x8)(u64x2){v, 0}, z = {0};
  return __builtin_convertvector((s32x4)__builtin_shufflevector(z, h, 0, 8, 1, 9, 2, 10, 3, 11),
                                 f32x4);
}
MX_INLINE f32x4 skin_nrm_in(const s8x4 *n) {
  s8x16 b = (s8x16)(s32x4){*(const s32 *)n, 0, 0, 0}, z = {0};
  s16x8 h = (s16x8)__builtin_shufflevector(z, b, 0, 16, 1, 17, 2, 18, 3, 19, 4, 20, 5, 21, 6, 22,
                                           7, 23),
        hz = {0};
  return __builtin_convertvector((s32x4)__builtin_shufflevector(hz, h, 0, 8, 1, 9, 2, 10, 3, 11),
                                 f32x4);
}
MX_INLINE s8x4 skin_nrm_out(f32x4 t) {
  return __builtin_convertvector(__builtin_convertvector(t, s32x4), s8x4);
}
MX_INLINE f32x4 skin_unit(f32x4 t) {
  f32x4 t2 = t * t;
  return t * (127.0f / __builtin_sqrtf(t2[0] + t2[1] + t2[2] + 1e-30f));
}

__attribute__((noinline)) static void macro_one(f32x4 *__restrict o, s8x4 *__restrict om, u32 n) {
  _Pragma("clang loop unroll_count(2) pipeline(disable)") for (u32 i = 0; i < n; i++) {
    const f32x4 *m = pm[jnt[i] & 255];
    f32x4 p = skin_pos_in(pos + 3 * i);
    o[i] = m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3];
    f32x4 nv = skin_nrm_in(nrm + i);
    om[i] = skin_nrm_out(skin_unit(m[0] * nv[0] + m[1] * nv[1] + m[2] * nv[2]));
  }
}

__attribute__((noinline)) static void macro_four(f32x4 *__restrict o, s8x4 *__restrict om, u32 n) {
  _Pragma("clang loop unroll_count(2) pipeline(disable)") for (u32 i = 0; i < n; i++) {
    u32 jb = jnt[i];
    const f32x4 *a = pm[jb & 255], *b = pm[jb >> 8 & 255];
    const f32x4 *c = pm[jb >> 16 & 255], *d = pm[jb >> 24];
    f32x4 w = __builtin_convertvector(wgt[i], f32x4) * (1.0f / 255.0f);
    f32x4 c0 = a[0] * w[0] + b[0] * w[1] + c[0] * w[2] + d[0] * w[3];
    f32x4 c1 = a[1] * w[0] + b[1] * w[1] + c[1] * w[2] + d[1] * w[3];
    f32x4 c2 = a[2] * w[0] + b[2] * w[1] + c[2] * w[2] + d[2] * w[3];
    f32x4 c3 = a[3] * w[0] + b[3] * w[1] + c[3] * w[2] + d[3] * w[3];
    f32x4 p = skin_pos_in(pos + 3 * i);
    o[i] = c0 * p[0] + c1 * p[1] + c2 * p[2] + c3;
    f32x4 nv = skin_nrm_in(nrm + i);
    om[i] = skin_nrm_out(skin_unit(c0 * nv[0] + c1 * nv[1] + c2 * nv[2]));
  }
}

/* ---- micro mode through VF registers ------------------------------------- */

MX_INLINE s8x4 reg_m(s32x4 m) { return __builtin_convertvector(m, s8x4); }

#define VCALLMS(a) __asm__ volatile(".word %0" : : "i"(0x4a000038 | (a) << 6) : "memory")
#define VU0_WAIT() __asm__ volatile(".word 0x4848e801" : : : "$8", "memory") /* cfc2.i $8, vi29 */

/* Vertex i into set A (vf1..vf4) or B (vf7..vf10) as MMI words: P q * 65536
   (PEXTLH with 0), N n * 2^24, J and W the bytes zero-extended (PEXTLB,
   PEXTLH with 0). */
#define REG_IN(i, p, n, j, w)                                                                      \
  __asm__ volatile("ldl $8, 7(%0)\n\tldr $8, 0(%0)\n\tlw $9, 0(%1)\n\tlw $10, 0(%2)\n\t"           \
                   "lw $11, 0(%3)\n\tpextlh $8, $8, $0\n\tpextlb $9, $9, $0\n\t"                   \
                   "pextlb $10, $0, $10\n\tpextlb $11, $0, $11\n\tpextlh $9, $9, $0\n\t"           \
                   "pextlh $10, $0, $10\n\tpextlh $11, $0, $11\n\t"                                \
                   "qmtc2 $8, $" p "\n\tqmtc2 $9, $" n "\n\tqmtc2 $10, $" j "\n\tqmtc2 $11, $" w   \
                   :                                                                               \
                   : "r"(pos + 3 * (i)), "r"(&nrm[i]), "r"(&jnt[i]), "r"(&wgt[i])                  \
                   : "$8", "$9", "$10", "$11")
/* The results of the set's last vertex (vf5, vf6 or vf11, vf12) to o, om:
   SQC2; QMFC2, PPACH, PPACB, SW. */
#define REG_OUT(i, r, m)                                                                           \
  __asm__ volatile("sqc2 $" r ", 0(%0)\n\tqmfc2 $8, $" m "\n\tppach $8, $0, $8\n\t"                 \
                   "ppacb $8, $0, $8\n\tsw $8, 0(%1)"                                              \
                   :                                                                               \
                   : "r"(&o[i]), "r"(&om[i])                                                       \
                   : "$8", "memory")

__attribute__((noinline)) static void micro_reg(f32x4 *__restrict o, s8x4 *__restrict om, u32 n) {
  for (u32 i = 0; i < n; i += 2) {
    REG_IN(i, "vf1", "vf2", "vf3", "vf4");
    if (i >= 2)
      REG_OUT(i - 2, "vf5", "vf6");
    VCALLMS(VS0_A);
    REG_IN(i + 1, "vf7", "vf8", "vf9", "vf10");
    if (i >= 2)
      REG_OUT(i - 1, "vf11", "vf12");
    VCALLMS(VS0_B);
  }
  VU0_WAIT();
  REG_OUT(n - 2, "vf5", "vf6");
  REG_OUT(n - 1, "vf11", "vf12");
}

/* Pipelined (vs0_pa, vs0_pb): a call ends the other set's vertex, so a
   normal comes out one call after its position. Even vertices go to set A,
   odd ones to B; the last call (set A, vertex 0 again) only ends n - 1. */
#define REG_R(i, r) __asm__ volatile("sqc2 $" r ", 0(%0)" : : "r"(&o[i]) : "memory")
#define REG_M(i, m)                                                                                \
  __asm__ volatile("qmfc2 $8, $" m "\n\tppach $8, $0, $8\n\tppacb $8, $0, $8\n\tsw $8, 0(%0)"     \
                   :                                                                               \
                   : "r"(&om[i])                                                                   \
                   : "$8", "memory")

__attribute__((noinline)) static void micro_pipe(f32x4 *__restrict o, s8x4 *__restrict om, u32 n) {
  REG_IN(0, "vf1", "vf2", "vf3", "vf4");
  VCALLMS(VS0_PA);
  for (u32 i = 1; i < n; i += 2) {
    REG_IN(i, "vf7", "vf8", "vf9", "vf10");
    VCALLMS(VS0_PB);
    REG_R(i - 1, "vf5");
    if (i >= 2)
      REG_M(i - 2, "vf12");
    u32 k = i + 1 < n ? i + 1 : 0;
    REG_IN(k, "vf1", "vf2", "vf3", "vf4");
    VCALLMS(VS0_PA);
    REG_R(i, "vf11");
    REG_M(i - 1, "vf6");
  }
  VU0_WAIT();
  REG_M(n - 1, "vf12");
}


/* Where the time of micro_reg goes: calls only, transfers only, one side. */
__attribute__((noinline)) static void d_call_empty(f32x4 *o, s8x4 *om, u32 n) {
  (void)o, (void)om;
  for (u32 i = 0; i < n; i++)
    VCALLMS(VS0_INIT);
  VU0_WAIT();
}
__attribute__((noinline)) static void d_call_only(f32x4 *o, s8x4 *om, u32 n) {
  (void)o, (void)om;
  for (u32 i = 0; i < n; i += 2) {
    VCALLMS(VS0_A);
    VCALLMS(VS0_B);
  }
  VU0_WAIT();
}
__attribute__((noinline)) static void d_pipe_calls(f32x4 *o, s8x4 *om, u32 n) {
  (void)o, (void)om;
  for (u32 i = 0; i < n; i += 2) {
    VCALLMS(VS0_PA);
    VCALLMS(VS0_PB);
  }
  VU0_WAIT();
}
__attribute__((noinline)) static void d_in_only(f32x4 *o, s8x4 *om, u32 n) {
  (void)o, (void)om;
  for (u32 i = 0; i < n; i += 2) {
    REG_IN(i, "vf1", "vf2", "vf3", "vf4");
    REG_IN(i + 1, "vf7", "vf8", "vf9", "vf10");
  }
}
__attribute__((noinline)) static void d_out_only(f32x4 *__restrict o, s8x4 *__restrict om, u32 n) {
  for (u32 i = 0; i < n; i += 2) {
    REG_OUT(i, "vf5", "vf6");
    REG_OUT(i + 1, "vf11", "vf12");
  }
}
__attribute__((noinline)) static void d_xfer_only(f32x4 *__restrict o, s8x4 *__restrict om, u32 n) {
  for (u32 i = 0; i < n; i += 2) {
    REG_IN(i, "vf1", "vf2", "vf3", "vf4");
    REG_OUT(i, "vf5", "vf6");
    REG_IN(i + 1, "vf7", "vf8", "vf9", "vf10");
    REG_OUT(i + 1, "vf11", "vf12");
  }
}
__attribute__((noinline)) static void d_in_call(f32x4 *o, s8x4 *om, u32 n) {
  (void)o, (void)om;
  for (u32 i = 0; i < n; i += 2) {
    REG_IN(i, "vf1", "vf2", "vf3", "vf4");
    VCALLMS(VS0_A);
    REG_IN(i + 1, "vf7", "vf8", "vf9", "vf10");
    VCALLMS(VS0_B);
  }
  VU0_WAIT();
}
__attribute__((noinline)) static void d_call_out(f32x4 *__restrict o, s8x4 *__restrict om, u32 n) {
  for (u32 i = 0; i < n; i += 2) {
    REG_OUT(i, "vf5", "vf6");
    VCALLMS(VS0_A);
    REG_OUT(i + 1, "vf11", "vf12");
    VCALLMS(VS0_B);
  }
  VU0_WAIT();
}

/* micro_pipe with the input streams prefetched 256 bytes ahead (PREF 0). */
#define PF(i)                                                                                      \
  __asm__ volatile("pref 0, 256(%0)\n\tpref 0, 256(%1)\n\tpref 0, 256(%2)\n\tpref 0, 256(%3)"     \
                   :                                                                               \
                   : "r"(pos + 3 * (i)), "r"(&nrm[i]), "r"(&jnt[i]), "r"(&wgt[i]))
__attribute__((noinline)) static void micro_pipe_pf(f32x4 *__restrict o, s8x4 *__restrict om, u32 n) {
  REG_IN(0, "vf1", "vf2", "vf3", "vf4");
  VCALLMS(VS0_PA);
  for (u32 i = 1; i < n; i += 2) {
    REG_IN(i, "vf7", "vf8", "vf9", "vf10");
    VCALLMS(VS0_PB);
    if ((i & 7) == 1) {
      PF(i);
      __asm__ volatile("pref 0, 256(%0)\n\tpref 0, 128(%0)\n\tpref 0, 256(%1)" : : "r"(&o[i]), "r"(&om[i]));
    }
    REG_R(i - 1, "vf5");
    if (i >= 2)
      REG_M(i - 2, "vf12");
    u32 k = i + 1 < n ? i + 1 : 0;
    REG_IN(k, "vf1", "vf2", "vf3", "vf4");
    VCALLMS(VS0_PA);
    REG_R(i, "vf11");
    REG_M(i - 1, "vf6");
  }
  VU0_WAIT();
  REG_M(n - 1, "vf12");
}

/* ---- micro mode through VIF0 DMA ---------------------------------------- */

/* Batch b's chain: ITOP n, the four streams unpacked, MSCAL. */
static void build_chain(u32 b) {
  u64 *c = chains[b];
  u32 f = b * BATCH;
  c = mx_e_ref(c, pos + 3 * f, BATCH * 6 / 16, mx_vif_stcycl(1, 1),
               mx_vif_unpack(MX_UNPACK_V3_16, 128, BATCH, 0));
  c = mx_e_ref(c, nrm + f, BATCH * 4 / 16, mx_vif_itop(BATCH),
               mx_vif_unpack(MX_UNPACK_V4_8, 160, BATCH, 0));
  c = mx_e_ref(c, jnt + f, BATCH * 4 / 16, 0, mx_vif_unpack(MX_UNPACK_V4_8, 192, BATCH, MX_UNPACK_USN));
  c = mx_e_ref(c, wgt + f, BATCH * 4 / 16, 0, mx_vif_unpack(MX_UNPACK_V4_8, 224, BATCH, MX_UNPACK_USN));
  c = mx_e_end(c, 0, mx_vif_mscal(VS0_BATCH), 0);
}

static u32 t_dma, t_read;
#define SPR_BUF ((void *)MX_SPR)

__attribute__((noinline)) static void micro_dma(f32x4 *__restrict o, s8x4 *__restrict om, u32 n) {
  for (u32 b = 0; b < n / BATCH; b++) {
    u32 t0 = mx_count();
    mx_dma_chain(MX_CH_VIF0, chains[b], 1);
    mx_dma_wait(MX_CH_VIF0);
    while (MX_R32(MX_VIF_VIF0_STAT) & 3)
      ;
    VU0_WAIT();
    u32 t1 = mx_count();
    mx_vu0_read_dma(o + b * BATCH, 128, BATCH, SPR_BUF, 0x2000);
    mx_vu0_read_dma(mtmp, 160, BATCH, SPR_BUF, 0x2000);
    for (u32 k = 0; k < BATCH; k++)
      om[b * BATCH + k] = reg_m(mtmp[k]);
    t_dma += t1 - t0, t_read += mx_count() - t1;
  }
}

/* ---- data, checks ---------------------------------------------------------- */

static void make_data(int four) {
  for (u32 i = 0; i < 3 * N + 8; i++)
    pos[i] = (s16)(rnd() & 0xFFFF);
  for (u32 i = 0; i < N; i++) {
    s8 a, b, c;
    do
      a = (s8)(rnd() % 255 - 127), b = (s8)(rnd() % 255 - 127), c = (s8)(rnd() % 255 - 127);
    while (a * a + b * b + c * c < 400);
    nrm[i] = (s8x4){a, b, c, 0};
    u32 s[4];
    for (int k = 0; k < 4; k++)
      s[k] = rnd() % JOINTS;
    jnt[i] = s[0] | s[1] << 8 | s[2] << 16 | s[3] << 24;
    if (four) {
      u32 w0 = 64 + rnd() % 128, w1 = rnd() % (256 - w0), w2 = rnd() % (256 - w0 - w1);
      u32 w3 = 255 - w0 - w1 - w2;
      wgt[i] = (u8x4){(u8)w0, (u8)w1, (u8)w2, (u8)w3};
    } else
      wgt[i] = (u8x4){255, 0, 0, 0};
  }
  const float sc = 1.0f / 32768.0f / 65536.0f;
  for (u32 j = 0; j < JOINTS; j++) {
    for (int k = 0; k < 3; k++)
      pm[j][k] = (f32x4){frnd(), frnd(), frnd(), 0} * sc;
    pm[j][3] = (f32x4){frnd(), frnd(), frnd(), 1};
    for (int k = 0; k < 4; k++) {
      vpal[0][32 * k + j] = pm[j][k];
      vpal[1][32 * k + j] = k < 3 ? pm[j][k] * 65536.0f : pm[j][k];
    }
  }
  mx_dcache_writeback(pos, sizeof pos);
  mx_dcache_writeback(nrm, sizeof nrm);
  mx_dcache_writeback(jnt, sizeof jnt);
  mx_dcache_writeback(wgt, sizeof wgt);
}

static float fabsf_(float a) { return a < 0 ? -a : a; }

/* Results of method k against macro mode's (op[0]): positions that differ
   by more than 2^-18 of their size, normals by more than 1. */
static void check(int k, u32 *bad_pos, u32 *bad_nrm) {
  *bad_pos = *bad_nrm = 0;
  for (u32 i = 0; i < N; i++) {
    for (int f = 0; f < 3; f++) {
      float a = op[0][i][f], b = op[k][i][f];
      float m = fabsf_(a) > 1 ? fabsf_(a) : 1;
      if (fabsf_(a - b) > m * (1.0f / 262144.0f) && (*bad_pos)++ < 3)
        mx_printf("  v %u pos[%d]: %d / 1000 vs %d / 1000\n", i, f, (int)(a * 1000), (int)(b * 1000));
      int d = on[0][i][f] - on[k][i][f];
      if ((d > 1 || d < -1) && (*bad_nrm)++ < 3)
        mx_printf("  v %u nrm[%d]: %d vs %d\n", i, f, on[0][i][f], on[k][i][f]);
    }
  }
}

static u32 best(void (*f)(f32x4 *, s8x4 *, u32), int k) {
  u32 b = ~0u;
  for (int r = 0; r < 3; r++) {
    t_dma = t_read = 0;
    u32 t0 = mx_count();
    f(op[k], on[k], N);
    u32 t = mx_count() - t0;
    if (t < b)
      b = t;
  }
  return b;
}

/* VU0 alone: one batch already in its memory, MSCAL to the end. */
static u32 kernel_alone(void) {
  u32 b = ~0u;
  for (int r = 0; r < 3; r++) {
    mx_dma_chain(MX_CH_VIF0, chains[0], 1);
    mx_dma_wait(MX_CH_VIF0);
    while (MX_R32(MX_VIF_VIF0_STAT) & 3)
      ;
    VU0_WAIT();
    u32 t0 = mx_count();
    VCALLMS(VS0_BATCH);
    VU0_WAIT();
    u32 t = mx_count() - t0;
    if (t < b)
      b = t;
  }
  return b;
}

int main(void) {
  mx_vu_reset(1);
  mx_vu_load(0, vskin0_vu, (u32)(vskin0_vu_end - vskin0_vu), 0);
  VCALLMS(VS0_INIT);
  VU0_WAIT();
  for (u32 b = 0; b < N / BATCH; b++)
    build_chain(b);
  mx_dcache_writeback(chains, sizeof chains);
  int fail = 0;
  for (int four = 0; four < 2; four++) {
    make_data(four);
    const char *kind = four ? "four joints" : "one joint";
    u32 tm = best(four ? macro_four : macro_one, 0);
    u32 tp = mx_count();
    mx_vu0_write(0, vpal[0], PAL_QW);
    tp = mx_count() - tp;
    u32 tr = best(micro_reg, 1);
    u32 tq = best(micro_pipe, 3);
    mx_vu0_write(0, vpal[1], PAL_QW);
    u32 td = best(micro_dma, 2);
    u32 ta = kernel_alone();
    mx_printf("vu0skin %s, %u vertices, EE cycles a vertex (x10):\n", kind, N);
    mx_printf("  macro mode            %u\n", tm * 10 / N);
    mx_printf("  micro, VF registers   %u\n", tr * 10 / N);
    mx_printf("  ... pipelined         %u\n", tq * 10 / N);
    mx_printf("  micro, VIF0 DMA       %u (in + run %u, readback %u)\n", td * 10 / N,
              t_dma * 10 / N, t_read * 10 / N);
    mx_printf("  VU0 kernel alone      %u (a batch of %u in its memory)\n", ta * 10 / BATCH, BATCH);
    mx_printf("  palette upload (COP2) %u cycles for %u qw\n", tp, PAL_QW);
    for (int k = 1; k < 4; k++) {
      u32 bp, bn;
      check(k, &bp, &bn);
      mx_printf("  %s against macro mode: %u positions, %u normals differ\n",
                k == 1 ? "registers" : k == 2 ? "VIF0 DMA " : "pipelined", bp, bn);
      fail |= bp || bn;
    }
    mx_printf("  diag: empty call %u, A/B calls only %u, transfers only %u\n",
              best(d_call_empty, 1) * 10 / N, best(d_call_only, 1) * 10 / N,
              best(d_xfer_only, 1) * 10 / N);
    mx_printf("  diag: in + call %u, call + out %u, pa/pb calls only %u\n", best(d_in_call, 1) * 10 / N,
              best(d_call_out, 1) * 10 / N, best(d_pipe_calls, 1) * 10 / N);
    mx_printf("  diag: in only %u, out only %u\n", best(d_in_only, 1) * 10 / N, best(d_out_only, 1) * 10 / N);
    mx_vu0_write(0, vpal[0], PAL_QW);
    u32 tf = best(micro_pipe_pf, 1);
    u32 bp, bn;
    check(1, &bp, &bn);
    mx_printf("  pipelined + prefetch %u: %u positions, %u normals differ\n", tf * 10 / N, bp, bn);
    fail |= bp || bn;
  }
  MX_TEST_END(fail);
  return 0;
}
