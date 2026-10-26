/* VU0 micro mode skinning fed through VF registers, by joint count (one,
   two, four: probe/vskin1.vsm, pipelined over two register sets), with the
   vertices read three ways: from main memory, from the scratchpad (toSPR
   chains of C vertices, double-buffered; the results written to main memory
   by the EE, as the model demo could: its fromSPR feeds the MFIFO ring), and
   with the results through the scratchpad too (fromSPR destination chains).
   Each against model.c's macro mode loop of the joint count; EE cycles a
   vertex. */
#include <mx/mx.h>

typedef float f32x4 __attribute__((vector_size(16)));
typedef s32 s32x4 __attribute__((vector_size(16)));
typedef s16 s16x8 __attribute__((vector_size(16)));
typedef s8 s8x16 __attribute__((vector_size(16)));
typedef u64 u64x2 __attribute__((vector_size(16)));
typedef u8 u8x4 __attribute__((vector_size(4)));
typedef s8 s8x4 __attribute__((vector_size(4)));

extern const u8 vskin1_vu[], vskin1_vu_end[];
#define VS1_INIT 0
#define VS1_P1A 64
#define VS1_P1B 128
#define VS1_P2A 192
#define VS1_P2B 256
#define VS1_P4A 320
#define VS1_P4B 384
#define PAL_QW 128 /* 4 columns x 32 slots */

#define N 4096
#define JOINTS 24
#define C 64    /* vertices of a scratchpad chunk */
#define M_OUT 4 /* methods: macro, memory, scratchpad in, scratchpad in and out */
/* the streams 640 bytes apart in a 4 KB D-cache way (p15_vu0skin.c) */
static struct {
  s16 pos[3 * N + 8];
  u8 pad1[624];
  s8x4 nrm[N];
  u8 pad2[640];
  u32 jnt[N];
  u8 pad3[640];
  u8x4 wgt[N];
  u8 pad4[640];
  f32x4 op[M_OUT][N];
  u8 pad5[640];
  s8x4 on[M_OUT][N];
} D MX_ALIGNED(64);
#define pos D.pos
#define nrm D.nrm
#define jnt D.jnt
#define wgt D.wgt
#define op D.op
#define on D.on
static f32x4 pm[JOINTS][4] MX_ALIGNED(64);
static f32x4 vpal[PAL_QW] MX_ALIGNED(64);
static u64x2 in_tags[N / C][4] MX_ALIGNED(64);

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
  s16x8 h = (s16x8)__builtin_shufflevector(z, b, 0, 16, 1, 17, 2, 18, 3, 19, 4, 20, 5, 21, 6, 22, 7,
                                           23),
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
#define SKIN_LOOP _Pragma("clang loop unroll_count(2) pipeline(disable)")

__attribute__((noinline)) static void macro_1(f32x4 *__restrict o, s8x4 *__restrict om, u32 n) {
  SKIN_LOOP for (u32 i = 0; i < n; i++) {
    const f32x4 *m = pm[jnt[i] & 255];
    f32x4 p = skin_pos_in(pos + 3 * i);
    o[i] = m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3];
    f32x4 nv = skin_nrm_in(nrm + i);
    om[i] = skin_nrm_out(skin_unit(m[0] * nv[0] + m[1] * nv[1] + m[2] * nv[2]));
  }
}
__attribute__((noinline)) static void macro_2(f32x4 *__restrict o, s8x4 *__restrict om, u32 n) {
  SKIN_LOOP for (u32 i = 0; i < n; i++) {
    u32 jb = jnt[i];
    const f32x4 *a = pm[jb & 255], *b = pm[jb >> 8 & 255];
    f32x4 w = __builtin_convertvector(wgt[i], f32x4) * (1.0f / 255.0f);
    f32x4 c0 = a[0] * w[0] + b[0] * w[1], c1 = a[1] * w[0] + b[1] * w[1];
    f32x4 c2 = a[2] * w[0] + b[2] * w[1], c3 = a[3] * w[0] + b[3] * w[1];
    f32x4 p = skin_pos_in(pos + 3 * i);
    o[i] = c0 * p[0] + c1 * p[1] + c2 * p[2] + c3;
    f32x4 nv = skin_nrm_in(nrm + i);
    om[i] = skin_nrm_out(skin_unit(c0 * nv[0] + c1 * nv[1] + c2 * nv[2]));
  }
}
__attribute__((noinline)) static void macro_4(f32x4 *__restrict o, s8x4 *__restrict om, u32 n) {
  SKIN_LOOP for (u32 i = 0; i < n; i++) {
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

#define VCALLMS(a) __asm__ volatile(".word %0" : : "i"(0x4a000038 | (a) << 6) : "memory")
#define VU0_WAIT() __asm__ volatile(".word 0x4848e801" : : : "$8", "memory") /* cfc2.i $8, vi29 */

/* A vertex into set A (vf1..vf4) or B (vf7..vf10) as MMI words: P q * 65536
   (PEXTLH with 0), N n * 2^24; four and two joints: J and W the bytes
   zero-extended (PEXTLB, PEXTLH with 0); one joint: J the raw word (the
   program takes its low byte), no W. */
/* IN_SET(IN, ..., SET_A): the set's names as IN's last four arguments */
#define IN_SET(IN, ...) IN(__VA_ARGS__)
#define SET_A "vf1", "vf2", "vf3", "vf4"
#define SET_B "vf7", "vf8", "vf9", "vf10"
#define IN4(P, Nn, J, W, p, n, j, w)                                                               \
  __asm__ volatile("ldl $8, 7(%0)\n\tldr $8, 0(%0)\n\tlw $9, 0(%1)\n\tlw $10, 0(%2)\n\t"           \
                   "lw $11, 0(%3)\n\tpextlh $8, $8, $0\n\tpextlb $9, $9, $0\n\t"                   \
                   "pextlb $10, $0, $10\n\tpextlb $11, $0, $11\n\tpextlh $9, $9, $0\n\t"           \
                   "pextlh $10, $0, $10\n\tpextlh $11, $0, $11\n\t"                                \
                   "qmtc2 $8, $" p "\n\tqmtc2 $9, $" n "\n\tqmtc2 $10, $" j "\n\tqmtc2 $11, $" w   \
                   :                                                                               \
                   : "r"(P), "r"(Nn), "r"(J), "r"(W)                                               \
                   : "$8", "$9", "$10", "$11")
#define IN1(P, Nn, J, W, p, n, j, w)                                                               \
  (void)(W);                                                                                       \
  __asm__ volatile("ldl $8, 7(%0)\n\tldr $8, 0(%0)\n\tlw $9, 0(%1)\n\tlw $10, 0(%2)\n\t"           \
                   "pextlh $8, $8, $0\n\tpextlb $9, $9, $0\n\tpextlh $9, $9, $0\n\t"               \
                   "qmtc2 $8, $" p "\n\tqmtc2 $9, $" n "\n\tqmtc2 $10, $" j                        \
                   :                                                                               \
                   : "r"(P), "r"(Nn), "r"(J)                                                       \
                   : "$8", "$9", "$10")
#define IN2 IN4

/* The outputs: R of a set's vertex (SQC2), M (QMFC2, PPACH, PPACB, SW). */
#define R_TO(at, r) __asm__ volatile("sqc2 $" r ", 0(%0)" : : "r"(at) : "memory")
#define M_TO(at, m)                                                                                \
  __asm__ volatile("qmfc2 $8, $" m "\n\tppach $8, $0, $8\n\tppacb $8, $0, $8\n\tsw $8, 0(%0)"      \
                   :                                                                               \
                   : "r"(at)                                                                       \
                   : "$8", "memory")

/* The vertex streams of vertex i: in main memory, or in the scratchpad
   chunk (C vertices: pos 6 C bytes, nrm, jnt, wgt 4 C each; two buffers). */
#define CH_POS 0
#define CH_NRM (6 * C)
#define CH_JNT (10 * C)
#define CH_WGT (14 * C)
#define CH_BYTES (18 * C)
#define SPR_IN ((u8 *)MX_SPR)
#define SPR_OUT ((u8 *)MX_SPR + 0x1000)
#define OUT_BYTES (16 + 16 * C + 16 + 4 * C) /* a cnt tag, R, an end tag, M */
#define AT_MEM(i, P, Nn, J, W)                                                                     \
  const void *P = pos + 3 * (i), *Nn = &nrm[i], *J = &jnt[i], *W = &wgt[i]
#define AT_SPR(i, P, Nn, J, W)                                                                     \
  const u8 *P##b = SPR_IN + ((i) / C & 1) * CH_BYTES;                                              \
  u32 P##o = (i) % C;                                                                              \
  const void *P = P##b + CH_POS + 6 * P##o, *Nn = P##b + CH_NRM + 4 * P##o,                        \
             *J = P##b + CH_JNT + 4 * P##o, *W = P##b + CH_WGT + 4 * P##o

/* toSPR: chunk c of the streams into its buffer (source chain, prebuilt). */
static void chunk_in(u32 c) {
  MX_DMA_REG(MX_CH_TOSPR, SADR) = (u32)(c & 1) * CH_BYTES;
  MX_DMA_REG(MX_CH_TOSPR, QWC) = 0;
  MX_DMA_REG(MX_CH_TOSPR, TADR) = mx_phys(in_tags[c]);
  mx_sync();
  MX_DMA_REG(MX_CH_TOSPR, CHCR) = MX_CHCR_CHAIN | MX_CHCR_STR;
}
static void make_in_tags(void) {
  for (u32 c = 0; c < N / C; c++) {
    u32 f = c * C;
    in_tags[c][0] = (u64x2){mx_dmatag(MX_TAG_REF, 6 * C / 16, pos + 3 * f, 0), 0};
    in_tags[c][1] = (u64x2){mx_dmatag(MX_TAG_REF, 4 * C / 16, &nrm[f], 0), 0};
    in_tags[c][2] = (u64x2){mx_dmatag(MX_TAG_REF, 4 * C / 16, &jnt[f], 0), 0};
    in_tags[c][3] = (u64x2){mx_dmatag(MX_TAG_REFE, 4 * C / 16, &wgt[f], 0), 0};
  }
  mx_dcache_writeback(in_tags, sizeof in_tags);
}
/* fromSPR: chunk c's results (its buffer of four) to o, om (destination
   chain: cnt R, end M). */
static u8 *out_buf(u32 c) { return SPR_OUT + (c & 3) * OUT_BYTES; }
static void chunk_out(u32 c, f32x4 *o, s8x4 *om) {
  u8 *b = out_buf(c);
  mx_dma_wait(MX_CH_FROMSPR);
  *(u64x2 *)b = (u64x2){mx_dmatag(MX_TAG_CNT, C, o + c * C, 0), 0};
  *(u64x2 *)(b + 16 + 16 * C) = (u64x2){mx_dmatag(MX_TAG_END, C / 4, om + c * C, 0), 0};
  mx_dma_from_spr_chain(b);
}
#define R_MEM(i, r) R_TO(&o[i], r)
#define M_MEM(i, m) M_TO(&om[i], m)
#define R_SPR(i, r) R_TO(out_buf((i) / C) + 16 + 16 * ((i) % C), r)
#define M_SPR(i, m) M_TO(out_buf((i) / C) + 32 + 16 * C + 4 * ((i) % C), m)

/* At vertex k (even), before its inputs are read: a new chunk begins. */
#define HOOK_NONE(k)
#define HOOK_IN(k)                                                                                 \
  if ((k) % C == 0) {                                                                              \
    mx_dma_wait(MX_CH_TOSPR);                                                                      \
    if ((k) / C + 1 < n / C)                                                                       \
      chunk_in((k) / C + 1);                                                                       \
  }
#define HOOK_INOUT(k)                                                                              \
  HOOK_IN(k)                                                                                       \
  if ((k) % C == 0 && (k) >= 2 * C)                                                                \
    chunk_out((k) / C - 2, o, om);
#define PRE_NONE
#define PRE_IN                                                                                     \
  chunk_in(0);                                                                                     \
  mx_dma_wait(MX_CH_TOSPR);                                                                        \
  if (n > C)                                                                                       \
    chunk_in(1);
#define POST_NONE
#define POST_OUT                                                                                   \
  for (u32 c = n / C >= 2 ? n / C - 2 : 0; c < n / C; c++)                                         \
    chunk_out(c, o, om);                                                                           \
  mx_dma_wait(MX_CH_FROMSPR);

/* The pipelined loop: even vertices in set A (program pa), odd ones in B
   (pb); a call ends the other set's vertex, so M comes out one call after
   R. The last call (set A, vertex n - 1 again) only ends n - 1. */
#define PIPE(name, IN, PA, PB, AT, R, M, PRE, HOOK, POST)                                          \
  __attribute__((noinline)) static void name(f32x4 *__restrict o, s8x4 *__restrict om, u32 n) {    \
    PRE {                                                                                          \
      AT(0, p, q, j, w);                                                                           \
      IN_SET(IN, p, q, j, w, SET_A);                                                               \
    }                                                                                              \
    VCALLMS(PA);                                                                                   \
    for (u32 i = 1; i < n; i += 2) {                                                               \
      {                                                                                            \
        AT(i, p, q, j, w);                                                                         \
        IN_SET(IN, p, q, j, w, SET_B);                                                             \
      }                                                                                            \
      VCALLMS(PB);                                                                                 \
      R(i - 1, "vf5");                                                                             \
      if (i >= 2)                                                                                  \
        M(i - 2, "vf12");                                                                          \
      u32 k = i + 1 < n ? i + 1 : i;                                                               \
      if (k != i) {                                                                                \
        HOOK(k)                                                                                    \
      }                                                                                            \
      {                                                                                            \
        AT(k, p, q, j, w);                                                                         \
        IN_SET(IN, p, q, j, w, SET_A);                                                             \
      }                                                                                            \
      VCALLMS(PA);                                                                                 \
      R(i, "vf11");                                                                                \
      M(i - 1, "vf6");                                                                             \
    }                                                                                              \
    VU0_WAIT();                                                                                    \
    M(n - 1, "vf12");                                                                              \
    POST                                                                                           \
  }

#define PIPES(J)                                                                                   \
  PIPE(mem_##J, IN##J, VS1_P##J##A, VS1_P##J##B, AT_MEM, R_MEM, M_MEM, PRE_NONE, HOOK_NONE,        \
       POST_NONE)                                                                                  \
  PIPE(spr_##J, IN##J, VS1_P##J##A, VS1_P##J##B, AT_SPR, R_MEM, M_MEM, PRE_IN, HOOK_IN, POST_NONE) \
  PIPE(spr2_##J, IN##J, VS1_P##J##A, VS1_P##J##B, AT_SPR, R_SPR, M_SPR, PRE_IN, HOOK_INOUT,        \
       POST_OUT)
PIPES(1)
PIPES(2)
PIPES(4)

/* The parts apart: the programs alone (back to back), the EE's inputs alone
   from memory and from the scratchpad (no DMA: the first chunks). */
#define CALLS(J)                                                                                   \
  __attribute__((noinline)) static void calls_##J(f32x4 *o, s8x4 *om, u32 n) {                     \
    (void)o, (void)om;                                                                             \
    for (u32 i = 0; i < n; i += 2) {                                                               \
      VCALLMS(VS1_P##J##A);                                                                        \
      VCALLMS(VS1_P##J##B);                                                                        \
    }                                                                                              \
    VU0_WAIT();                                                                                    \
  }                                                                                                \
  __attribute__((noinline)) static void inmem_##J(f32x4 *o, s8x4 *om, u32 n) {                     \
    (void)o, (void)om;                                                                             \
    for (u32 i = 0; i < n; i++) {                                                                  \
      AT_MEM(i, p, q, j, w);                                                                       \
      IN_SET(IN##J, p, q, j, w, SET_A);                                                            \
    }                                                                                              \
  }                                                                                                \
  __attribute__((noinline)) static void inspr_##J(f32x4 *o, s8x4 *om, u32 n) {                     \
    (void)o, (void)om;                                                                             \
    for (u32 i = 0; i < n; i++) {                                                                  \
      AT_SPR(i % (2 * C), p, q, j, w);                                                             \
      IN_SET(IN##J, p, q, j, w, SET_A);                                                            \
    }                                                                                              \
  }
CALLS(1)
CALLS(2)
CALLS(4)

/* ---- data, checks ---------------------------------------------------------- */

/* joints: 1 (weights 255, 0, 0, 0), 2 (two of 255 together), 4 */
static void make_data(int joints) {
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
    u32 w0 = 255, w1 = 0, w2 = 0, w3 = 0;
    if (joints == 2)
      w0 = 128 + rnd() % 127, w1 = 255 - w0;
    else if (joints == 4)
      w0 = 64 + rnd() % 128, w1 = rnd() % (256 - w0), w2 = rnd() % (256 - w0 - w1),
      w3 = 255 - w0 - w1 - w2;
    wgt[i] = (u8x4){(u8)w0, (u8)w1, (u8)w2, (u8)w3};
  }
  const float sc = 1.0f / 32768.0f / 65536.0f;
  for (u32 j = 0; j < JOINTS; j++) {
    for (int k = 0; k < 3; k++)
      pm[j][k] = (f32x4){frnd(), frnd(), frnd(), 0} * sc;
    pm[j][3] = (f32x4){frnd(), frnd(), frnd(), 1};
    for (int k = 0; k < 4; k++)
      vpal[32 * k + j] = pm[j][k];
  }
  mx_dcache_writeback(&D, sizeof D);
}

static float fabsf_(float a) { return a < 0 ? -a : a; }

/* Method k against macro mode (op[0]): positions that differ by more than
   2^-18 of their size, normals by more than 1. */
static u32 check(int k) {
  u32 bad = 0;
  for (u32 i = 0; i < N; i++)
    for (int f = 0; f < 3; f++) {
      float a = op[0][i][f], b = op[k][i][f];
      float m = fabsf_(a) > 1 ? fabsf_(a) : 1;
      int d = on[0][i][f] - on[k][i][f];
      if ((fabsf_(a - b) > m * (1.0f / 262144.0f) || d > 1 || d < -1) && bad++ < 3)
        mx_printf("  method %d v %u [%d]: pos %d vs %d / 1000, nrm %d vs %d\n", k, i, f,
                  (int)(a * 1000), (int)(b * 1000), on[0][i][f], on[k][i][f]);
    }
  return bad;
}

/* The best of three runs, in EE cycles a vertex x 10. The outputs cleared
   first, out of the cache (fromSPR writes them), and written back after
   (the EE's). */
static u32 best(void (*f)(f32x4 *, s8x4 *, u32), int k) {
  u32 b = ~0u;
  for (int r = 0; r < 3; r++) {
    for (u32 i = 0; i < N; i++)
      op[k][i] = (f32x4){0}, on[k][i] = (s8x4){0};
    mx_dcache_flush_all();
    u32 t0 = mx_count();
    f(op[k], on[k], N);
    u32 t = mx_count() - t0;
    mx_dcache_flush_all(); /* the EE's results out, fromSPR's not hidden */
    if (t < b)
      b = t;
  }
  return b * 10 / N;
}

typedef void fn(f32x4 *, s8x4 *, u32);
static const struct {
  int joints;
  fn *macro, *mem, *spr, *spr2, *calls, *inmem, *inspr;
} kinds[] = {
    {1, macro_1, mem_1, spr_1, spr2_1, calls_1, inmem_1, inspr_1},
    {2, macro_2, mem_2, spr_2, spr2_2, calls_2, inmem_2, inspr_2},
    {4, macro_4, mem_4, spr_4, spr2_4, calls_4, inmem_4, inspr_4},
};

int main(void) {
  mx_vu_reset(1);
  mx_vu_load(0, vskin1_vu, (u32)(vskin1_vu_end - vskin1_vu), 0);
  VCALLMS(VS1_INIT);
  VU0_WAIT();
  make_in_tags();
  int fail = 0;
  mx_printf("vu0reg: %u vertices, EE cycles a vertex (x10); chunks of %u\n", N, C);
  for (u32 t = 0; t < sizeof kinds / sizeof kinds[0]; t++) {
    make_data(kinds[t].joints);
    mx_vu0_write(0, vpal, PAL_QW);
    u32 tm = best(kinds[t].macro, 0);
    u32 tr = best(kinds[t].mem, 1), ts = best(kinds[t].spr, 2), t2 = best(kinds[t].spr2, 3);
    u32 bad[M_OUT] = {0};
    for (int k = 1; k < M_OUT; k++)
      fail |= (bad[k] = check(k)) != 0;
    /* the parts (the outputs are not checked) */
    chunk_in(0);
    mx_dma_wait(MX_CH_TOSPR);
    chunk_in(1);
    mx_dma_wait(MX_CH_TOSPR);
    u32 tc = best(kinds[t].calls, 1), ti = best(kinds[t].inmem, 1), tj = best(kinds[t].inspr, 1);
    mx_printf("vu0reg %u joint%s: macro %u, registers: memory %u, SPR in %u, SPR in+out %u; "
              "differ %u %u %u\n",
              kinds[t].joints, kinds[t].joints > 1 ? "s" : "", tm, tr, ts, t2, bad[1], bad[2],
              bad[3]);
    mx_printf("vu0reg %u joint%s parts: VU0 programs %u, EE inputs from memory %u, from SPR %u\n",
              kinds[t].joints, kinds[t].joints > 1 ? "s" : "", tc, ti, tj);
  }
  MX_TEST_END(fail);
  return 0;
}
