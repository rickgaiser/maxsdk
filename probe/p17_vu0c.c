/* VU0 micro skinning fed through VF registers (probe/p16_vu0reg.c) with the
   compiler's VU0 additions (docs/llvm-issues.md 16-18): VCALLMS and CFC2.I
   of VI registers as instructions, -ffixed-vf1 .. -ffixed-vf31 (this file:
   the programs of demos/model/vu0skin.vsm write most VF registers while
   they run, so no code in between may use any), and the input step in C
   (the MMI zero-extensions with $zero). Two feeders, both with the inputs
   from the scratchpad (toSPR chains of C vertices, double-buffered):
     A  p16's asm blocks (loads, MMI, QMTC2), now with the mnemonics
     B  the input and output steps in C, only QMTC2 / QMFC2 / SQC2 in asm
   for one, two and four joints, against a scalar (FPU) reference; EE
   cycles a vertex. No VU0 macro mode here: with all VF registers reserved
   there is none to compute in. */
#include <mx/mx.h>

typedef s32 s32x4 __attribute__((vector_size(16)));
typedef s16 s16x8 __attribute__((vector_size(16)));
typedef s8 s8x16 __attribute__((vector_size(16)));
typedef u8 u8x16 __attribute__((vector_size(16)));
typedef u64 u64x2 __attribute__((vector_size(16)));
typedef s8 s8x4 __attribute__((vector_size(4)));

extern const u8 vu0skin_vu[], vu0skin_vu_end[];
/* the programs, in instructions (VCALLMS takes bytes) */
#define VS_INIT 0
#define VS_P1A 64
#define VS_P1B 128
#define VS_P2A 192
#define VS_P2B 256
#define VS_P4A 320
#define VS_P4B 384
#define PAL_QW 128 /* 4 columns x 32 slots */

#define N 4096
#define JOINTS 24
#define C 64     /* vertices of a scratchpad chunk */
#define M_OUT 3  /* the reference, A, B */
/* the streams 640 bytes apart in a 4 KB D-cache way (p15_vu0skin.c) */
static struct {
  s16 pos[3 * N + 8];
  u8 pad1[624];
  s8 nrm[N][4];
  u8 pad2[640];
  u32 jnt[N];
  u8 pad3[640];
  u8 wgt[N][4];
  u8 pad4[640];
  float op[M_OUT][N][4];
  u8 pad5[640];
  s8 on[M_OUT][N][4];
} D MX_ALIGNED(64);
#define pos D.pos
#define nrm D.nrm
#define jnt D.jnt
#define wgt D.wgt
#define op D.op
#define on D.on
static float pm[JOINTS][4][4];             /* joint j's columns (as model.c's je) */
static float vpal[PAL_QW][4] MX_ALIGNED(64); /* column k of slot j at 32 k + j */
static u64x2 in_tags[N / C][4] MX_ALIGNED(64);

static u32 seed = 1;
static u32 rnd(void) { return seed = seed * 1103515245u + 12345u, seed >> 8; }
static float frnd(void) { return (float)(s32)(rnd() & 0xFFFF) / 32768.0f - 1.0f; }

/* ---- the reference: model.c's skinning in scalar floats ---------------------- */

static void reference(int joints) {
  for (u32 i = 0; i < N; i++) {
    float c[4][3] = {{0}};
    for (int s = 0; s < (joints == 1 ? 1 : 4); s++) {
      const float(*m)[4] = pm[jnt[i] >> 8 * s & 255];
      float w = joints == 1 ? 1.0f : (float)wgt[i][s] * (1.0f / 255.0f);
      for (int k = 0; k < 4; k++)
        for (int f = 0; f < 3; f++)
          c[k][f] += m[k][f] * w;
    }
    float p[3], t[3];
    for (int f = 0; f < 3; f++)
      p[f] = (float)(s32)((u32)(u16)pos[3 * i + f] << 16), t[f] = (float)(s32)((u32)(u8)nrm[i][f] << 24);
    float u[3], l2 = 0;
    for (int f = 0; f < 3; f++) {
      op[0][i][f] = c[0][f] * p[0] + c[1][f] * p[1] + c[2][f] * p[2] + c[3][f];
      u[f] = c[0][f] * t[0] + c[1][f] * t[1] + c[2][f] * t[2];
      l2 += u[f] * u[f];
    }
    float k = 127.0f / __builtin_sqrtf(l2);
    for (int f = 0; f < 3; f++)
      on[0][i][f] = (s8)(s32)(u[f] * k);
  }
}

/* ---- the calls ------------------------------------------------------------------ */

#define VCALLMS(a) __asm__ volatile("vcallms %0" : : "i"(8 * (a)) : "memory")
#define VU0_WAIT() __asm__ volatile("cfc2.i $8, $vi29" : : : "$8", "memory")

/* the scratchpad chunks (two): pos 6 C bytes, nrm, jnt, wgt 4 C each */
#define CH_NRM (6 * C)
#define CH_JNT (10 * C)
#define CH_WGT (14 * C)
#define CH_BYTES (18 * C)
#define SPR_IN ((u8 *)MX_SPR)
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
    in_tags[c][1] = (u64x2){mx_dmatag(MX_TAG_REF, 4 * C / 16, nrm[f], 0), 0};
    in_tags[c][2] = (u64x2){mx_dmatag(MX_TAG_REF, 4 * C / 16, &jnt[f], 0), 0};
    in_tags[c][3] = (u64x2){mx_dmatag(MX_TAG_REFE, 4 * C / 16, wgt[f], 0), 0};
  }
  mx_dcache_writeback(in_tags, sizeof in_tags);
}
/* vertex i's streams in its chunk */
#define AT(i, P, Nn, J, W)                                                                         \
  const u8 *P##b = SPR_IN + ((i) / C & 1) * CH_BYTES;                                              \
  u32 P##o = (i) % C;                                                                              \
  const u8 *P = P##b + 6 * P##o;                                                                   \
  const u32 *Nn = (const u32 *)(P##b + CH_NRM) + P##o, *J = (const u32 *)(P##b + CH_JNT) + P##o,   \
            *W = (const u32 *)(P##b + CH_WGT) + P##o

/* ---- feeder A: asm blocks (p16) ---------------------------------------------- */

#define IN_SET(IN, ...) IN(__VA_ARGS__)
#define SET_A "vf1", "vf2", "vf3", "vf4"
#define SET_B "vf7", "vf8", "vf9", "vf10"
#define A_IN4(P, Nn, J, W, p, n, j, w)                                                             \
  __asm__ volatile("ldl $8, 7(%0)\n\tldr $8, 0(%0)\n\tlw $9, 0(%1)\n\tlw $10, 0(%2)\n\t"           \
                   "lw $11, 0(%3)\n\tpextlh $8, $8, $0\n\tpextlb $9, $9, $0\n\t"                   \
                   "pextlb $10, $0, $10\n\tpextlb $11, $0, $11\n\tpextlh $9, $9, $0\n\t"           \
                   "pextlh $10, $0, $10\n\tpextlh $11, $0, $11\n\t"                                \
                   "qmtc2 $8, $" p "\n\tqmtc2 $9, $" n "\n\tqmtc2 $10, $" j "\n\tqmtc2 $11, $" w   \
                   :                                                                               \
                   : "r"(P), "r"(Nn), "r"(J), "r"(W)                                               \
                   : "$8", "$9", "$10", "$11")
#define A_IN1(P, Nn, J, W, p, n, j, w)                                                             \
  (void)(W);                                                                                       \
  __asm__ volatile("ldl $8, 7(%0)\n\tldr $8, 0(%0)\n\tlw $9, 0(%1)\n\tlw $10, 0(%2)\n\t"           \
                   "pextlh $8, $8, $0\n\tpextlb $9, $9, $0\n\tpextlh $9, $9, $0\n\t"               \
                   "qmtc2 $8, $" p "\n\tqmtc2 $9, $" n "\n\tqmtc2 $10, $" j                        \
                   :                                                                               \
                   : "r"(P), "r"(Nn), "r"(J)                                                       \
                   : "$8", "$9", "$10")
#define A_IN2 A_IN4
#define A_R(i, r) __asm__ volatile("sqc2 $" r ", 0(%0)" : : "r"(o[i]) : "memory")
#define A_M(i, m)                                                                                  \
  __asm__ volatile("qmfc2 $8, $" m "\n\tppach $8, $0, $8\n\tppacb $8, $0, $8\n\tsw $8, 0(%0)"      \
                   :                                                                               \
                   : "r"(om[i])                                                                    \
                   : "$8", "memory")

/* ---- feeder B: the steps in C -------------------------------------------------- */

/* P q * 65536, N n * 2^24, J and W the bytes zero-extended */
MX_INLINE s32x4 b_pos(const u8 *p) {
  u64 v;
  __builtin_memcpy(&v, p, 8);
  s16x8 h = (s16x8)(u64x2){v, 0}, z = {0};
  return (s32x4)__builtin_shufflevector(z, h, 0, 8, 1, 9, 2, 10, 3, 11);
}
MX_INLINE s32x4 b_nrm(const u32 *n) {
  s8x16 b = (s8x16)(s32x4){(s32)*n, 0, 0, 0}, z = {0};
  s16x8 h = (s16x8)__builtin_shufflevector(z, b, 0, 16, 1, 17, 2, 18, 3, 19, 4, 20, 5, 21, 6, 22, 7,
                                           23),
        hz = {0};
  return (s32x4)__builtin_shufflevector(hz, h, 0, 8, 1, 9, 2, 10, 3, 11);
}
MX_INLINE s32x4 b_u8(const u32 *u) {
  u8x16 b = (u8x16)(s32x4){(s32)*u, 0, 0, 0}, z = {0};
  s16x8 h = (s16x8)__builtin_shufflevector(b, z, 0, 16, 1, 17, 2, 18, 3, 19, 4, 20, 5, 21, 6, 22, 7,
                                           23),
        hz = {0};
  return (s32x4)__builtin_shufflevector(h, hz, 0, 8, 1, 9, 2, 10, 3, 11);
}
#define QMTC2(v, r) __asm__ volatile("qmtc2 %0, $" r : : "r"(v))
#define B_IN4(P, Nn, J, W, p, n, j, w)                                                             \
  do {                                                                                             \
    s32x4 vp_ = b_pos(P), vn_ = b_nrm(Nn), vj_ = b_u8(J), vw_ = b_u8(W);                           \
    QMTC2(vp_, p);                                                                                 \
    QMTC2(vn_, n);                                                                                 \
    QMTC2(vj_, j);                                                                                 \
    QMTC2(vw_, w);                                                                                 \
  } while (0)
#define B_IN1(P, Nn, J, W, p, n, j, w)                                                             \
  do {                                                                                             \
    (void)(W);                                                                                     \
    s32x4 vp_ = b_pos(P), vn_ = b_nrm(Nn);                                                         \
    s32x4 vj_ = {(s32)*(J)};                                                                       \
    QMTC2(vp_, p);                                                                                 \
    QMTC2(vn_, n);                                                                                 \
    QMTC2(vj_, j);                                                                                 \
  } while (0)
#define B_IN2 B_IN4
#define B_R(i, r) __asm__ volatile("sqc2 $" r ", 0(%0)" : : "r"(o[i]) : "memory")
#define B_M(i, m)                                                                                  \
  do {                                                                                             \
    s32x4 v_;                                                                                      \
    __asm__ volatile("qmfc2 %0, $" m : "=r"(v_));                                                  \
    *(s8x4 *)om[i] = __builtin_convertvector(v_, s8x4);                                            \
  } while (0)

/* ---- the pipelined loop (p16's PIPE with the inputs from the scratchpad) ---------- */

#define PIPE(name, IN, PA, PB, R, M)                                                               \
  __attribute__((noinline)) static void name(float (*__restrict o)[4], s8(*__restrict om)[4],     \
                                             u32 n) {                                              \
    chunk_in(0);                                                                                   \
    mx_dma_wait(MX_CH_TOSPR);                                                                      \
    if (n > C)                                                                                     \
      chunk_in(1);                                                                                 \
    {                                                                                              \
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
      if (k != i && k % C == 0) {                                                                  \
        mx_dma_wait(MX_CH_TOSPR);                                                                  \
        if (k / C + 1 < n / C)                                                                     \
          chunk_in(k / C + 1);                                                                     \
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
  }
#define PIPES(J)                                                                                   \
  PIPE(a_##J, A_IN##J, VS_P##J##A, VS_P##J##B, A_R, A_M)                                           \
  PIPE(b_##J, B_IN##J, VS_P##J##A, VS_P##J##B, B_R, B_M)
PIPES(1)
PIPES(2)
PIPES(4)

/* ---- data, checks ---------------------------------------------------------------- */

static void make_data(int joints) {
  for (u32 i = 0; i < 3 * N + 8; i++)
    pos[i] = (s16)(rnd() & 0xFFFF);
  for (u32 i = 0; i < N; i++) {
    s8 a, b, c;
    do
      a = (s8)(rnd() % 255 - 127), b = (s8)(rnd() % 255 - 127), c = (s8)(rnd() % 255 - 127);
    while (a * a + b * b + c * c < 400);
    nrm[i][0] = a, nrm[i][1] = b, nrm[i][2] = c, nrm[i][3] = 0;
    u32 s[4];
    for (int k = 0; k < 4; k++)
      s[k] = rnd() % JOINTS;
    jnt[i] = s[0] | s[1] << 8 | s[2] << 16 | s[3] << 24;
    u32 w[4] = {255, 0, 0, 0};
    if (joints == 2)
      w[0] = 128 + rnd() % 127, w[1] = 255 - w[0];
    else if (joints == 4)
      w[0] = 64 + rnd() % 128, w[1] = rnd() % (256 - w[0]), w[2] = rnd() % (256 - w[0] - w[1]),
      w[3] = 255 - w[0] - w[1] - w[2];
    for (int k = 0; k < 4; k++)
      wgt[i][k] = (u8)w[k];
  }
  const float sc = 1.0f / 32768.0f / 65536.0f;
  for (u32 j = 0; j < JOINTS; j++)
    for (int k = 0; k < 4; k++) {
      for (int f = 0; f < 3; f++)
        pm[j][k][f] = k < 3 ? frnd() * sc : frnd();
      pm[j][k][3] = k == 3;
      for (int f = 0; f < 4; f++)
        vpal[32 * k + j][f] = pm[j][k][f];
    }
  mx_dcache_writeback(&D, sizeof D);
}

static float fabsf_(float a) { return a < 0 ? -a : a; }

/* Feeder k against the reference: positions that differ by more than 2^-18
   of their size, normals by more than 1. */
static u32 check(int k) {
  u32 bad = 0;
  for (u32 i = 0; i < N; i++)
    for (int f = 0; f < 3; f++) {
      float a = op[0][i][f], b = op[k][i][f];
      float m = fabsf_(a) > 1 ? fabsf_(a) : 1;
      int d = on[0][i][f] - on[k][i][f];
      if ((fabsf_(a - b) > m * (1.0f / 262144.0f) || d > 1 || d < -1) && bad++ < 3)
        mx_printf("  feeder %c v %u [%d]: pos %d vs %d / 1000, nrm %d vs %d\n", 'A' + k - 1, i, f,
                  (int)(a * 1000), (int)(b * 1000), on[0][i][f], on[k][i][f]);
    }
  return bad;
}

typedef void fn(float (*)[4], s8 (*)[4], u32);
/* the best of three runs, EE cycles a vertex x 10 */
static u32 best(fn *f, int k) {
  u32 b = ~0u;
  for (int r = 0; r < 3; r++) {
    __builtin_memset(op[k], 0, sizeof op[k]);
    __builtin_memset(on[k], 0, sizeof on[k]);
    mx_dcache_flush_all();
    u32 t0 = mx_count();
    f(op[k], on[k], N);
    u32 t = mx_count() - t0;
    mx_dcache_flush_all();
    if (t < b)
      b = t;
  }
  return b * 10 / N;
}

static const struct {
  int joints;
  fn *a, *b;
} kinds[] = {{1, a_1, b_1}, {2, a_2, b_2}, {4, a_4, b_4}};

int main(void) {
  mx_vu_reset(1);
  mx_vu_load(0, vu0skin_vu, (u32)(vu0skin_vu_end - vu0skin_vu), 0);
  VCALLMS(VS_INIT);
  VU0_WAIT();
  make_in_tags();
  int fail = 0;
  mx_printf("vu0c: %u vertices, EE cycles a vertex (x10); A asm blocks, B steps in C\n", N);
  for (u32 t = 0; t < sizeof kinds / sizeof kinds[0]; t++) {
    make_data(kinds[t].joints);
    reference(kinds[t].joints);
    mx_vu0_write(0, vpal, PAL_QW);
    u32 ta = best(kinds[t].a, 1), tb = best(kinds[t].b, 2);
    u32 ba = check(1), bb = check(2);
    fail |= ba || bb;
    mx_printf("vu0c %u joint%s: A %u, B %u; differ from the reference: A %u, B %u\n",
              kinds[t].joints, kinds[t].joints > 1 ? "s" : "", ta, tb, ba, bb);
  }
  MX_TEST_END(fail);
  return 0;
}
