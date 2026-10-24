/* The procgen shaders (demos, PLAN_PROCGEN P3/P4) as tests/ps2/shader.c
   does the quality levels: single batches through VIF1, for every vertex
   count of a batch, read back from VU1 memory and compared with the same
   math on the EE. */

#include "dustdraw.h"
#include "t.h"
#include "tf0.h"
#include "tf1.h"
#include "tf2.h"
#include "tree.h"
#include "water.h"

extern const u8 water_vu[], water_vu_end[], tf0_vu[], tf0_vu_end[], tf1_vu[], tf1_vu_end[],
    tf2_vu[], tf2_vu_end[], tree_vu[], tree_vu_end[], dustdraw_vu[], dustdraw_vu_end[];

#define N 128
static float pos[N + 16][3] MX_ALIGNED(16);
static float pos4[N + 16][4] MX_ALIGNED(16);
static s8 nrm[N + 16][4] MX_ALIGNED(16);
static u32 col[N + 16] MX_ALIGNED(16);
static s16 uv[N + 16][2] MX_ALIGNED(16);
static float uni[16][4] MX_ALIGNED(16);
static u64 chain[2 * 64] MX_ALIGNED(64);
static u32 out[1 + 4 * N][4] MX_ALIGNED(64);

static u32 seed = 1;
static u32 rnd32(void) {
  seed = seed * 1103515245u + 12345u;
  return seed;
}
static float rnd(void) { return (float)(s32)(rnd32() >> 9 & 0xFFFF) / 32768.0f - 1.0f; }

static float fmaxf_(float a, float b) { return a > b ? a : b; }
static float fminf_(float a, float b) { return a < b ? a : b; }
static float fabsf_(float a) { return a < 0 ? -a : a; }

/* sin(x) for |x| < 100: x - 2 pi k in [-pi, pi], then the series to x^15 */
static float sin_(float x) {
  const float pi = 3.14159265f;
  s32 k = (s32)(x / (2 * pi) + (x < 0 ? -0.5f : 0.5f));
  x -= (float)k * 2 * pi;
  float x2 = x * x, t = x, s = x;
  for (int i = 1; i < 8; i++) {
    t *= -x2 / (float)((2 * i) * (2 * i + 1));
    s += t;
  }
  return s;
}

/* One batch of n vertices of s from pos; returns 0 when the program has
   ended. */
static int run(const struct mx_shader *s, u32 n, u32 *top) {
  static u32 mark = 0x300;
  mark = (mark + 1) & 0xFFFF;
  /* the inputs in the order of the shader: pos, [nrm], col, [uv] */
  const void *streams[4] = {pos, nrm, col, uv};
  if (s->nin == 2)
    streams[1] = col;
  if (s->in[0].bytes == 16)
    streams[0] = pos4;
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

static int bad;
static void near_int(const char *name, const char *what, u32 n, u32 i, int k, u32 got, s32 want,
                     s32 tol) {
  s32 d = (s32)got - want;
  if ((d > tol || -d > tol) && bad++ < 6)
    mx_printf("%s n %u vertex %u: %s[%d] = %d, expected %d\n", name, n, i, what, k, (s32)got, want);
}

/* p = mvp * (x, y, z, 1) for the first four uniforms (columns) */
static void transform(float p[4], float x, float y, float z) {
  for (int k = 0; k < 4; k++)
    p[k] = uni[0][k] * x + uni[1][k] * y + uni[2][k] * z + uni[3][k];
}

/* water.hlsl: uni 4 fogc, 5 wave, 6 wcol, 7 cull */
static void check_water(const struct mx_shader *s, u32 n, u32 top) {
  mx_vu1_read_dma(out, top + VU_WATER_OUT_AT, 1 + s->out_qw * n, (void *)0x70000000, 16384);
  CHECK_EQ(out[0][0] & 0x7FFF, n);
  float ex1 = 0, ex2 = 0;
  for (u32 i = 0; i < n; i++) {
    u32(*o)[4] = &out[1 + s->out_qw * i];
    float *w = uni[5], wx = pos[i][0] + w[2], wz = pos[i][2] + w[3];
    float s1 = sin_(wx * 0.11f + wz * 0.07f + w[0]);
    float s2 = sin_(wz * 0.13f - wx * 0.05f + w[0] * 1.3f);
    float y = (s1 + s2 * 0.6f) * w[1], p[4];
    transform(p, pos[i][0], pos[i][1] + y, pos[i][2]);
    float fog = fminf_(fmaxf_(p[3] * uni[4][0] + uni[4][1], 0), 255);
    float *cl = uni[7];
    float ox = fabsf_(p[0] - p[3] * cl[0]) - p[3] * cl[1];
    float oy = fabsf_(p[1] - p[3] * cl[0]) - p[3] * cl[1];
    float ex = fmaxf_(fmaxf_(ox, oy), cl[2] - p[3]);
    float worst = fmaxf_(ex, fmaxf_(ex1, ex2));
    int edge =
        fabsf_(ex) < 0.01f || (i > 0 && fabsf_(ex1) < 0.01f) || (i > 1 && fabsf_(ex2) < 0.01f);
    ex2 = ex1, ex1 = ex;
    for (int k = 0; k < 3; k++)
      near_int("water", "rgbaq", n, i, k, o[0][k], (s32)(uni[6][k] + (s1 + s2) * 14), 1);
    near_int("water", "rgbaq", n, i, 3, o[0][3], (s32)uni[6][3], 0);
    for (int k = 0; k < 3; k++)
      near_int("water", "xyzf2", n, i, k, o[1][k], (s32)(p[k] / p[3] * 16.0f), 2);
    near_int("water", "fog", n, i, 3, o[1][3] & 0x7FFF, (s32)(fog * 16.0f), 4);
    if (!edge)
      near_int("water", "adc", n, i, 3, o[1][3] >> 15 & 1, worst > 0, 0);
  }
}

/* tf0-2.hlsl: lvl0-2 of tests/ps2/shader.c with fog in xyzf2.w; uni 4 fogc,
   5 light, 6 lcol, 7 amb */
static void check_tf(u32 lvl, const struct mx_shader *s, u32 out_at, u32 n, u32 top) {
  static const char *names[3] = {"tf0", "tf1", "tf2"};
  const char *name = names[lvl];
  mx_vu1_read_dma(out, top + out_at, 1 + s->out_qw * n, (void *)0x70000000, 16384);
  CHECK_EQ(out[0][0] & 0x7FFF, n);
  for (u32 i = 0; i < n; i++) {
    u32(*o)[4] = &out[1 + s->out_qw * i];
    float p[4], cf[4], nf[4];
    transform(p, pos[i][0], pos[i][1], pos[i][2]);
    float q = 1.0f / p[3];
    for (int k = 0; k < 4; k++) {
      cf[k] = (float)(s32)(col[i] >> (8 * k) & 0xFF);
      nf[k] = (float)nrm[i][k];
    }
    float fog = fminf_(fmaxf_(p[3] * uni[4][0] + uni[4][1], 0), 255);
    u32 r = lvl == 2;
    if (lvl == 0) {
      for (int k = 0; k < 4; k++)
        near_int(name, "rgbaq", n, i, k, o[0][k], (s32)cf[k], 0);
    } else {
      float d = fmaxf_(nf[0] * uni[5][0] + nf[1] * uni[5][1] + nf[2] * uni[5][2], 0);
      for (int k = 0; k < 4; k++)
        near_int(name, "rgbaq", n, i, k, o[r][k],
                 (s32)fminf_(cf[k] * (uni[7][k] + uni[6][k] * d), 255), 2);
    }
    if (r) {
      union {
        u32 u;
        float f;
      } st = {o[0][2]};
      near_int(name, "q", n, i, 2, (u32)(s32)(st.f * 4096.0f), (s32)(q * 4096.0f), 2);
    }
    for (int k = 0; k < 3; k++)
      near_int(name, "xyzf2", n, i, k, o[r + 1][k], (s32)(p[k] * q * 16.0f), 2);
    near_int(name, "fog", n, i, 3, o[r + 1][3], (s32)(fog * 16.0f), 4);
  }
}

/* tree.hlsl: two GS vertices per point; uni 4 fogc, 5 bb, 6 uvc, 7 cull */
static void check_tree(const struct mx_shader *s, u32 arg, u32 n, u32 top) {
  mx_vu1_read_dma(out, top + VU_TREE_OUT_AT, 1 + s->out_qw * n, (void *)0x70000000, 16384);
  CHECK_EQ(out[0][0] & 0x7FFF, n);
  for (u32 i = 0; i < n; i++) {
    u32(*o)[4] = &out[1 + s->out_qw * i];
    float p[4], c[4];
    transform(p, pos[i][0], pos[i][1], pos[i][2]);
    float q = 1.0f / p[3];
    for (int k = 0; k < 4; k++)
      c[k] = p[k] * q;
    float fog = fminf_(fmaxf_(p[3] * uni[4][0] + uni[4][1], 0), 255);
    float hw = q * uni[5][0], h = q * uni[5][1], *cl = uni[7];
    float ex =
        fmaxf_(fmaxf_(fabsf_(c[0] - cl[0]) + hw, fabsf_(c[1] - cl[0]) + h) - cl[1], cl[2] - p[3]);
    const float *uvc = uni[6];
    float want[4][4] = {{uvc[0], uvc[1], 0, 0},
                        {c[0] - hw, c[1] - h, c[2], fog},
                        {uvc[2], uvc[3], 0, 0},
                        {c[0] + hw, c[1], c[2], fog}};
    for (int r = 0; r < 4; r++)
      for (int k = 0; k < 4; k++)
        near_int("tree", r & 1 ? "xyzf2" : "uv", n, i, r * 4 + k,
                 r == 3 && k == 3 ? o[r][k] & 0x7FFF : o[r][k], (s32)(want[r][k] * 16.0f),
                 r & 1 ? 4 : 0);
    if (fabsf_(ex) > 0.01f)
      near_int("tree", "adc", n, i, 15, o[3][3] >> 15 & 1, ex > 0, 0);
  }
}

/* floor(v) by truncation */
static float floor_(float v) {
  float t = (float)(s32)v;
  return t > v ? t - 1 : t;
}

/* dustdraw.hlsl: a mote (pos4, V4-32) as a sprite; uni 4 fogc, 5 box,
   6 ext, 7 col, 8 cull */
static void check_dustdraw(const struct mx_shader *s, u32 arg, u32 n, u32 top) {
  mx_vu1_read_dma(out, top + VU_DUSTDRAW_OUT_AT, 1 + s->out_qw * n, (void *)0x70000000, 16384);
  CHECK_EQ(out[0][0] & 0x7FFF, n);
  for (u32 i = 0; i < n; i++) {
    u32(*o)[4] = &out[1 + s->out_qw * i];
    float q[4], p[4], c[4];
    for (int k = 0; k < 4; k++) {
      q[k] = pos4[i][k] - uni[5][k];
      q[k] = (q[k] - floor_(q[k])) * uni[5][3];
    }
    transform(p, q[0], q[1] + uni[6][0], q[2]);
    float r = 1.0f / p[3];
    for (int k = 0; k < 4; k++)
      c[k] = p[k] * r;
    float fog = fminf_(fmaxf_(p[3] * uni[4][0] + uni[4][1], 0), 255);
    float hw = r * uni[6][1], hh = r * uni[6][2], *cl = uni[8];
    float ex =
        fmaxf_(fmaxf_(fabsf_(c[0] - cl[0]) + hw, fabsf_(c[1] - cl[0]) + hh) - cl[1], cl[2] - p[3]);
    for (int k = 0; k < 4; k++)
      near_int("dustdraw", "rgbaq", n, i, k, o[0][k], (s32)uni[7][k], 0);
    float want[2][4] = {{c[0] - hw, c[1] - hh, c[2], fog}, {c[0] + hw, c[1] + hh, c[2], fog}};
    for (int v = 0; v < 2; v++)
      for (int k = 0; k < 4; k++)
        near_int("dustdraw", "xyzf2", n, i, v * 4 + k, k == 3 ? o[1 + v][k] & 0x7FFF : o[1 + v][k],
                 (s32)(want[v][k] * 16.0f), 4);
    if (fabsf_(ex) > 0.01f)
      near_int("dustdraw", "adc", n, i, 7, o[2][3] >> 15 & 1, ex > 0, 0);
  }
}

static void run_all(const char *name, const struct mx_shader *s,
                    void (*check)(const struct mx_shader *, u32, u32, u32), u32 arg) {
  mx_vu_reset(2);
  mx_vif_reset(1);
  mx_vu_forget(1);
  mx_vu_load(1, s->code, s->code_bytes, s->entry);
  bad = 0;
  for (u32 n = 1; n <= s->max_n && n <= N; n++) {
    u32 top;
    if (run(s, n, &top)) {
      mx_printf("%s n %u: the batch did not end\n", name, n);
      t_fail++;
      break;
    }
    check(s, arg, n, top);
  }
  if (bad)
    t_fail++;
  mx_printf("%s: max %u vertices per batch, %s\n", name, s->max_n, bad ? "FAILED" : "ok");
}

static void check_water_(const struct mx_shader *s, u32 arg, u32 n, u32 top) {
  check_water(s, n, top);
}
static void check_tf_(const struct mx_shader *s, u32 lvl, u32 n, u32 top) {
  static const u32 out_at[3] = {VU_TF0_OUT_AT, VU_TF1_OUT_AT, VU_TF2_OUT_AT};
  check_tf(lvl, s, out_at[lvl], n, top);
}

int main(void) {
  mx_gs_init(640, 224);
  mx_vif_reset(1);
  for (u32 i = 0; i < N + 16; i++) {
    pos[i][0] = rnd() * 8, pos[i][1] = rnd() * 0.25f, pos[i][2] = rnd() * 8;
    for (int k = 0; k < 3; k++)
      nrm[i][k] = (s8)(s32)(rnd() * 70);
    nrm[i][3] = 0;
    col[i] = rnd32();
    uv[i][0] = (s16)(rnd32() & 0x1FFF), uv[i][1] = (s16)(rnd32() & 0x1FFF);
  }
  mx_dcache_writeback(pos, sizeof pos);
  mx_dcache_writeback(nrm, sizeof nrm);
  mx_dcache_writeback(col, sizeof col);
  mx_dcache_writeback(uv, sizeof uv);

  static const float m[4][4] = {
      {100, 0, 0, 0}, {0, 80, 0, 0}, {0, 0, 10, 0.25f}, {0, 0, 3000, 2.0f}};
  for (int i = 0; i < 16; i++)
    for (int k = 0; k < 4; k++)
      uni[i][k] = i < 4 ? m[i][k] : 0;
  uni[4][0] = -300, uni[4][1] = 800;                                   /* fog */
  uni[5][0] = 1.25f, uni[5][1] = 0.5f, uni[5][2] = 10, uni[5][3] = 20; /* wave */
  uni[6][0] = 40, uni[6][1] = 80, uni[6][2] = 120, uni[6][3] = 96;     /* colour */
  uni[7][0] = 0, uni[7][1] = 300, uni[7][2] = 1.6f;                    /* cull */

  struct mx_shader water = MX_VU_SHADER(WATER, water_vu, water_vu_end);
  run_all("water", &water, check_water_, 0);

  /* tf1, tf2: light, light colour, ambient */
  static const float L[3] = {0.2575f, -0.4292f, -0.8584f};
  for (int k = 0; k < 4; k++)
    uni[5][k] = k < 3 ? L[k] / 127.0f : 0;
  uni[6][0] = 0.9f, uni[6][1] = 0.8f, uni[6][2] = 0.7f, uni[6][3] = 0;
  uni[7][0] = uni[7][1] = uni[7][2] = 0.2f, uni[7][3] = 1;
  struct mx_shader tf[3] = {MX_VU_SHADER(TF0, tf0_vu, tf0_vu_end),
                            MX_VU_SHADER(TF1, tf1_vu, tf1_vu_end),
                            MX_VU_SHADER(TF2, tf2_vu, tf2_vu_end)};
  static const char *names[3] = {"tf0", "tf1", "tf2"};
  for (u32 l = 0; l < 3; l++)
    run_all(names[l], &tf[l], check_tf_, l);

  /* tree: fogc, half width and height, texture corners, cull */
  uni[5][0] = 40, uni[5][1] = 90, uni[5][2] = uni[5][3] = 0;
  uni[6][0] = 0, uni[6][1] = 0, uni[6][2] = 32, uni[6][3] = 64;
  uni[7][0] = 0, uni[7][1] = 300, uni[7][2] = 1.6f, uni[7][3] = 0;
  struct mx_shader tree = MX_VU_SHADER(TREE, tree_vu, tree_vu_end);
  run_all("tree", &tree, check_tree, 0);

  /* dustdraw: motes in box units, the box at (0.25, 0, 0.5) of size 6 */
  for (u32 i = 0; i < N + 16; i++)
    for (int k = 0; k < 4; k++)
      pos4[i][k] = rnd() * 2;
  mx_dcache_writeback(pos4, sizeof pos4);
  uni[5][0] = 0.25f, uni[5][1] = 0, uni[5][2] = 0.5f, uni[5][3] = 6;
  uni[6][0] = 0.5f, uni[6][1] = 20, uni[6][2] = 15, uni[6][3] = 0;
  uni[7][0] = 255, uni[7][1] = 238, uni[7][2] = 200, uni[7][3] = 96;
  uni[8][0] = 0, uni[8][1] = 300, uni[8][2] = 1.6f, uni[8][3] = 0;
  struct mx_shader dd = MX_VU_SHADER(DUSTDRAW, dustdraw_vu, dustdraw_vu_end);
  run_all("dustdraw", &dd, check_dustdraw, 0);
  return t_done("shader2");
}
