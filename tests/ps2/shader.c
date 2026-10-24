/* HLSL shaders (PLAN_PROCGEN P2): single batches of the quality-level shaders
   (demos/procgen/lvl0-3.hlsl) through VIF1, for every vertex count of a batch (each
   takes its own way through the compiled loop: the unrolled kernel, the
   stages before and after it, the loop for the rest), read back from VU1
   memory and compared with the same math on the EE. */

#include "lvl0.h"
#include "lvl1.h"
#include "lvl2.h"
#include "lvl3.h"
#include "t.h"

extern const u8 lvl0_vu[], lvl0_vu_end[], lvl1_vu[], lvl1_vu_end[], lvl2_vu[], lvl2_vu_end[],
    lvl3_vu[], lvl3_vu_end[];

#define N 128
static float pos[N + 16][3] MX_ALIGNED(16);
static s8 nrm[N + 16][4] MX_ALIGNED(16);
static u32 col[N + 16] MX_ALIGNED(16);
static s16 uv[N + 16][2] MX_ALIGNED(16);
static float uni[16][4] MX_ALIGNED(16);
static u64 chain[2 * 64] MX_ALIGNED(64);
static u32 out[1 + 3 * N][4] MX_ALIGNED(64);

static u32 seed = 1;
static u32 rnd32(void) {
  seed = seed * 1103515245u + 12345u;
  return seed;
}
static float rnd(void) { return (float)(s32)(rnd32() >> 9 & 0xFFFF) / 32768.0f - 1.0f; }

struct level {
  const char *name;
  struct mx_shader s;
  u32 nuni;
};

/* One batch of n vertices; returns 0 when the program has ended. */
static int run(const struct mx_shader *s, u32 n, u32 *top) {
  static u32 mark = 0x200;
  mark = (mark + 1) & 0xFFFF;
  const void *all[4] = {pos, nrm, col, uv}, *streams[4];
  /* the inputs in the order of the shader: pos, [nrm], col, [uv] */
  u32 k = 0;
  streams[k++] = all[0];
  if (s->nin >= 3)
    streams[k++] = all[1];
  streams[k++] = all[2];
  if (s->nin == 4)
    streams[k++] = all[3];
  u64 *c = chain;
  c = mx_e_cnt(c, 1, mx_vif_base(0), mx_vif_offset(s->buf_qw));
  c = mx_e_vif4(c, mx_vif_stcycl(1, 1), 0, 0, 0);
  c = mx_shader_e_batch(c, s, mx_gs_prim(MX_GS_PRIM_TRISTRIP, 1, 0, 0, 0, 0, 0, 0, 0), uni, streams,
                        n);
  c = mx_e_end(c, 0, mx_vif_flush(), mx_vif_mark(mark));
  mx_dcache_writeback(chain, sizeof chain);
  mx_dcache_writeback(uni, sizeof uni);
  mx_dma_chain(MX_CH_VIF1, chain, 1);
  u32 t0 = mx_count();
  while ((MX_R32(MX_VIF_VIF1_MARK) & 0xFFFF) != mark)
    if (mx_count() - t0 > 100000000u)
      return -1;
  mx_dma_wait(MX_CH_VIF1);
  *top = MX_R32(MX_VIF_VIF1_TOP) & 0x3FF;
  return 0;
}

static float fmaxf_(float a, float b) { return a > b ? a : b; }
static float fminf_(float a, float b) { return a < b ? a : b; }
static float fabsf_(float a) { return a < 0 ? -a : a; }

static int bad;
static void near_int(const char *what, u32 lvl, u32 n, u32 i, int k, u32 got, s32 want, s32 tol) {
  s32 d = (s32)got - want;
  if ((d > tol || -d > tol) && bad++ < 6)
    mx_printf("lvl%u n %u vertex %u: %s[%d] = %d, expected %d\n", lvl, n, i, what, k, (s32)got,
              want);
}
static void near_float(const char *what, u32 lvl, u32 n, u32 i, int k, u32 got, float want) {
  union {
    u32 u;
    float f;
  } g = {got};
  float d = fabsf_(g.f - want);
  if (d > 1e-4f * fabsf_(want) + 1e-6f && bad++ < 6)
    mx_printf("lvl%u n %u vertex %u: %s[%d] = %d/1000, expected %d/1000\n", lvl, n, i, what, k,
              (s32)(g.f * 1000.0f), (s32)(want * 1000.0f));
}

/* The EE version of each level (the same expressions as the .hlsl files). */
static void check(u32 lvl, const struct mx_shader *s, u32 out_at, u32 n, u32 top) {
  mx_vu1_read_dma(out, top + out_at, 1 + s->out_qw * n, (void *)0x70000000, 16384);
  CHECK_EQ(out[0][0] & 0x7FFF, n);
  bad = 0;
  for (u32 i = 0; i < n; i++) {
    u32(*o)[4] = &out[1 + s->out_qw * i];
    float p[4], c[4];
    for (int k = 0; k < 4; k++)
      p[k] = uni[0][k] * pos[i][0] + uni[1][k] * pos[i][1] + uni[2][k] * pos[i][2] + uni[3][k];
    float q = 1.0f / p[3];
    float cf[4], nf[4];
    for (int k = 0; k < 4; k++) {
      cf[k] = (float)(s32)(col[i] >> (8 * k) & 0xFF);
      nf[k] = (float)nrm[i][k];
    }
    if (lvl == 0) {
      for (int k = 0; k < 4; k++)
        near_int("rgbaq", lvl, n, i, k, o[0][k], (s32)cf[k], 0);
      for (int k = 0; k < 4; k++)
        near_int("xyz2", lvl, n, i, k, o[1][k], (s32)(p[k] * q * 16.0f), 2);
      continue;
    }
    if (lvl <= 2) {
      /* uni 4 light, 5 lcol, 6 amb */
      float d = fmaxf_(nf[0] * uni[4][0] + nf[1] * uni[4][1] + nf[2] * uni[4][2], 0);
      for (int k = 0; k < 4; k++)
        c[k] = fminf_(cf[k] * (uni[6][k] + uni[5][k] * d), 255); /* RGBAQ keeps 8 bits */
      u32 r = lvl == 2;
      if (r) {
        near_float("st", lvl, n, i, 0, o[0][0], (float)uv[i][0] / 4096.0f * q);
        near_float("st", lvl, n, i, 1, o[0][1], (float)uv[i][1] / 4096.0f * q);
        near_float("st", lvl, n, i, 2, o[0][2], q);
      }
      for (int k = 0; k < 4; k++)
        near_int("rgbaq", lvl, n, i, k, o[r][k], (s32)c[k], 2);
      for (int k = 0; k < 4; k++)
        near_int("xyz2", lvl, n, i, k, o[r + 1][k], (s32)(p[k] * q * 16.0f), 2);
      continue;
    }
    /* level 3: uni 4-6 lm (4.w: the nearest w), 7-10 lc, 11 prm (fog a, b,
       centre, guard band) */
    float d[3];
    for (int j = 0; j < 3; j++)
      d[j] = fmaxf_(uni[4][j] * nf[0] + uni[5][j] * nf[1] + uni[6][j] * nf[2], 0);
    for (int k = 0; k < 4; k++)
      c[k] = fminf_(cf[k] * (uni[10][k] + uni[7][k] * d[0] + uni[8][k] * d[1] + uni[9][k] * d[2]),
                    255);
    float fog = fminf_(fmaxf_(p[3] * uni[11][0] + uni[11][1], 0), 255);
    float ox = fabsf_(p[0] - p[3] * uni[11][2]) - p[3] * uni[11][3];
    float oy = fabsf_(p[1] - p[3] * uni[11][2]) - p[3] * uni[11][3];
    float ex = fmaxf_(fmaxf_(ox, oy), uni[4][3] - p[3]);
    /* a strip triangle with any vertex out is not drawn: ADC from this
       vertex and the two before it in the batch */
    static float ex1, ex2;
    if (i == 0)
      ex1 = ex2 = 0;
    float worst = fmaxf_(ex, fmaxf_(ex1, ex2));
    int edge =
        fabsf_(ex) < 0.01f || (i > 0 && fabsf_(ex1) < 0.01f) || (i > 1 && fabsf_(ex2) < 0.01f);
    ex2 = ex1, ex1 = ex;
    near_float("st", lvl, n, i, 0, o[0][0], (float)uv[i][0] / 4096.0f * q);
    near_float("st", lvl, n, i, 1, o[0][1], (float)uv[i][1] / 4096.0f * q);
    near_float("st", lvl, n, i, 2, o[0][2], q);
    for (int k = 0; k < 4; k++)
      near_int("rgbaq", lvl, n, i, k, o[1][k], (s32)c[k], 2);
    for (int k = 0; k < 3; k++)
      near_int("xyzf2", lvl, n, i, k, o[2][k], (s32)(p[k] * q * 16.0f), 2);
    /* the fog and, away from the edge, the ADC bit */
    near_int("fog", lvl, n, i, 3, o[2][3] & 0x7FFF, (s32)(fog * 16.0f), 4);
    if (!edge)
      near_int("adc", lvl, n, i, 3, o[2][3] >> 15 & 1, worst > 0, 0);
  }
  if (bad)
    t_fail++;
}

static void set_uniforms(u32 lvl) {
  static const float m[4][4] = {
      {100, 0, 0, 0}, {0, 80, 0, 0}, {0, 0, 1000, 0.25f}, {3072, 3072, 3000, 1.5f}};
  for (int i = 0; i < 16; i++)
    for (int k = 0; k < 4; k++)
      uni[i][k] = i < 4 ? m[i][k] : 0;
  if (lvl == 1 || lvl == 2) {
    static const float L[3] = {0.2575f, -0.4292f, -0.8584f};
    for (int k = 0; k < 3; k++)
      uni[4][k] = L[k] / 127.0f;
    uni[5][0] = 0.9f, uni[5][1] = 0.8f, uni[5][2] = 0.7f;
    uni[6][0] = uni[6][1] = uni[6][2] = 0.2f, uni[6][3] = 1;
  } else if (lvl == 3) {
    for (int j = 0; j < 3; j++)
      for (int k = 0; k < 3; k++)
        uni[4 + k][j] = rnd() / 127.0f; /* lm[k] = component k of the three lights */
    for (int j = 0; j < 3; j++)
      for (int k = 0; k < 3; k++)
        uni[7 + j][k] = 0.3f + 0.2f * rnd();
    uni[10][0] = uni[10][1] = uni[10][2] = 0.1f, uni[10][3] = 1;
    uni[11][0] = -300, uni[11][1] = 700; /* fog 255 at w <= 1.48, 0 at w >= 2.33 */
    uni[11][2] = 0, uni[11][3] = 60;     /* centre, guard band: some vertices out */
    uni[4][3] = 1.4f;                    /* the nearest w: some vertices too near */
    /* centre the screen at 0 so that |X|, |Y| are the distances from it */
    uni[3][0] = uni[3][1] = 0;
  }
}

int main(void) {
  mx_gs_init(640, 224);
  mx_vif_reset(1);
  struct level lv[4] = {{"lvl0", MX_VU_SHADER(LVL0, lvl0_vu, lvl0_vu_end), VU_LVL0_UNI_QW},
                        {"lvl1", MX_VU_SHADER(LVL1, lvl1_vu, lvl1_vu_end), VU_LVL1_UNI_QW},
                        {"lvl2", MX_VU_SHADER(LVL2, lvl2_vu, lvl2_vu_end), VU_LVL2_UNI_QW},
                        {"lvl3", MX_VU_SHADER(LVL3, lvl3_vu, lvl3_vu_end), VU_LVL3_UNI_QW}};
  const u32 out_at[4] = {VU_LVL0_OUT_AT, VU_LVL1_OUT_AT, VU_LVL2_OUT_AT, VU_LVL3_OUT_AT};
  CHECK_EQ(lv[0].s.align, 4);
  for (u32 i = 0; i < N + 16; i++) {
    pos[i][0] = rnd(), pos[i][1] = rnd(), pos[i][2] = rnd();
    float a = rnd(), b = rnd(), c = rnd(), l = a * a + b * b + c * c + 1e-3f, r = 1;
    /* 1 / sqrt(l) by Newton steps (no libm) */
    for (int k = 0; k < 8; k++)
      r = r * (1.5f - 0.5f * l * r * r);
    nrm[i][0] = (s8)(s32)(a * r * 127), nrm[i][1] = (s8)(s32)(b * r * 127);
    nrm[i][2] = (s8)(s32)(c * r * 127), nrm[i][3] = 0;
    col[i] = rnd32();
    uv[i][0] = (s16)(rnd32() & 0x1FFF), uv[i][1] = (s16)(rnd32() & 0x1FFF);
  }
  mx_dcache_writeback(pos, sizeof pos);
  mx_dcache_writeback(nrm, sizeof nrm);
  mx_dcache_writeback(col, sizeof col);
  mx_dcache_writeback(uv, sizeof uv);

  for (u32 l = 0; l < 4; l++) {
    struct mx_shader *s = &lv[l].s;
    mx_vu_reset(2);
    mx_vif_reset(1);
    mx_vu_forget(1);
    mx_vu_load(1, s->code, s->code_bytes, s->entry);
    set_uniforms(l);
    int fails = t_fail;
    for (u32 n = 1; n <= s->max_n; n++) {
      u32 top;
      if (run(s, n, &top)) {
        mx_printf("%s n %u: the batch did not end\n", lv[l].name, n);
        t_fail++;
        break;
      }
      check(l, s, out_at[l], n, top);
    }
    mx_printf("%s: max %u vertices per batch, %s\n", lv[l].name, s->max_n,
              t_fail == fails ? "ok" : "FAILED");
  }
  return t_done("shader");
}
