/* Indexed HLSL vertex programs (opt -vu-indexed, llvm VUEntryABI.md): the
   quality-level shaders apps/procgen/lvl0-2.hlsl and the model demo's
   apps/model/m*.hlsl built indexed (<name>_ix, the HLSL_IX list of the
   Makefile), single batches through VIF1 for every vertex count and a set of
   index counts, with random indices. The packet must be, index by index, the
   outputs of the strip program for the same vertex, bit for bit. The batches
   alternate between the two buffers and the two uniform blocks; the other
   block holds other uniforms. The strip programs of lvl0-2 are checked
   against the EE by tests/ps2/shader.c; those of mskin (joint matrices from
   uniforms at run-time indices, integer vectors) and mscei (ps2gl's SCEI
   lighting) are checked here. */

#include "lvl0.h"
#include "lvl0_ix.h"
#include "lvl1.h"
#include "lvl1_ix.h"
#include "lvl2.h"
#include "lvl2_ix.h"
#include "mflat.h"
#include "mflat_ix.h"
#include "mlit.h"
#include "mlit_ix.h"
#include "mscei.h"
#include "mscei_ix.h"
#include "mskin.h"
#include "mskin_ix.h"
#include "mtex.h"
#include "mtex_ix.h"
#include "t.h"

extern const u8 lvl0_vu[], lvl0_vu_end[], lvl1_vu[], lvl1_vu_end[], lvl2_vu[], lvl2_vu_end[],
    lvl0_ix_vu[], lvl0_ix_vu_end[], lvl1_ix_vu[], lvl1_ix_vu_end[], lvl2_ix_vu[],
    lvl2_ix_vu_end[], mflat_vu[], mflat_vu_end[], mlit_vu[], mlit_vu_end[], mtex_vu[],
    mtex_vu_end[], mskin_vu[], mskin_vu_end[], mflat_ix_vu[], mflat_ix_vu_end[], mlit_ix_vu[],
    mlit_ix_vu_end[], mtex_ix_vu[], mtex_ix_vu_end[], mskin_ix_vu[], mskin_ix_vu_end[],
    mscei_vu[], mscei_vu_end[], mscei_ix_vu[], mscei_ix_vu_end[];

#define NV 64
#define NI 512
#define NUNI 112
#define NJOINT 24
static float pos[NV + 16][3] MX_ALIGNED(16);
static s16 pos16[NV + 16][3] MX_ALIGNED(16); /* mskin's V3-16: pos * 16384 */
static s8 nrm[NV + 16][4] MX_ALIGNED(16);
static u32 col[NV + 16] MX_ALIGNED(16);
static s16 uv[NV + 16][2] MX_ALIGNED(16);
static u8 jnt[NV + 16][4] MX_ALIGNED(16);
static u8 wgt[NV + 16][4] MX_ALIGNED(16);
static float uni[NUNI][4] MX_ALIGNED(16);
static float other[NUNI][4] MX_ALIGNED(16);
static u8 idx[NI + 16] MX_ALIGNED(16);
static u64 chain[2 * 320] MX_ALIGNED(64);
static u32 tab[1 + 3 * NV][4] MX_ALIGNED(64);
static u32 out[1 + 3 * NI + 16][4] MX_ALIGNED(64);

static u32 seed = 1;
static u32 rnd32(void) {
  seed = seed * 1103515245u + 12345u;
  return seed;
}
static float rnd(void) { return (float)(s32)(rnd32() >> 9 & 0xFFFF) / 32768.0f - 1.0f; }

static u32 prim; /* a Gouraud strip */

enum { POS, NRM, COL, UV, JNT, WGT, POS16 };
struct prog {
  const char *name;
  struct mx_shader strip, ix;
  int in[7], nin;           /* the streams, in the order of the inputs */
  int lit, tex, model;      /* uniforms: light (lvl or model style), the model demo's */
  void (*ref)(u32 n);       /* the EE version of the strip program, or 0 */
};

static const void *stream(int k) {
  static const void *const all[7] = {pos, nrm, col, uv, jnt, wgt, pos16};
  return all[k];
}

/* Send the chain, wait until VIF1 has passed the MARK at its end. */
static int send(u64 *c, u32 *top) {
  static u32 mark = 0x300;
  mark = (mark + 1) & 0xFFFF;
  c = mx_e_end(c, 0, mx_vif_flush(), mx_vif_mark(mark));
  if ((u8 *)c > (u8 *)chain + sizeof chain) {
    mx_printf("ishader: chain overflow\n");
    return -1;
  }
  mx_dcache_writeback(chain, sizeof chain);
  mx_dcache_writeback(uni, sizeof uni);
  mx_dcache_writeback(other, sizeof other);
  mx_dcache_writeback(idx, sizeof idx);
  mx_dma_chain(MX_CH_VIF1, chain, 1);
  u32 t0 = mx_count();
  while ((MX_R32(MX_VIF_VIF1_MARK) & 0xFFFF) != mark)
    if (mx_count() - t0 > 100000000u)
      return -1;
  mx_dma_wait(MX_CH_VIF1);
  if (top)
    *top = MX_R32(MX_VIF_VIF1_TOP) & 0x3FF;
  return 0;
}

/* The strip program on vertices 0 .. n - 1: their outputs into tab. */
static int run_strip(const struct prog *p, u32 n) {
  const struct mx_shader *s = &p->strip;
  const void *st[6];
  for (int i = 0; i < p->nin; i++)
    st[i] = stream(p->in[i]);
  u64 *c = chain;
  c = mx_e_cnt(c, 1, mx_vif_base(0), mx_vif_offset(s->buf_qw));
  c = mx_e_vif4(c, mx_vif_stcycl(1, 1), 0, 0, 0);
  c = mx_shader_e_context(c, s, mx_shader_ctx(s, 0), prim, uni);
  c = mx_shader_e_batch(c, s, st, n, 1);
  u32 top;
  if (send(c, &top))
    return -1;
  mx_vu1_read_dma(tab, top + s->out_at, 1 + s->out_qw * n, (void *)0x70000000, 16384);
  return 0;
}

/* One indexed batch of nv vertices and ni indices with the uniforms in
   block blk, in buffer buf (a first batch of one index into the other). */
static int run_indexed(const struct prog *p, u32 nv, u32 ni, u32 blk, u32 buf) {
  const struct mx_shader *s = &p->ix;
  const void *st[6];
  for (int i = 0; i < p->nin; i++)
    st[i] = stream(p->in[i]);
  u32 here = mx_shader_ctx(s, blk), there = mx_shader_ctx(s, blk ^ 1);
  u64 *c = chain;
  c = mx_e_cnt(c, 1, mx_vif_base(0), mx_vif_offset(s->buf_qw));
  c = mx_e_vif4(c, mx_vif_stcycl(1, 1), 0, 0, 0);
  /* the other context first: a batch runs with the context of the last
     init before it */
  c = mx_shader_e_context(c, s, there, prim, other);
  static u8 one[16] MX_ALIGNED(16);
  if (buf) {
    struct mx_ibatch f = {0, 1, 1, one};
    c = mx_shader_e_ibatch(c, s, st, &f, 1);
  }
  c = mx_shader_e_context(c, s, here, prim, uni);
  struct mx_ibatch b = {0, (u16)nv, (u16)ni, idx};
  c = mx_shader_e_ibatch(c, s, st, &b, 1);
  u32 top;
  if (send(c, &top))
    return -1;
  if (top != buf * s->buf_qw) {
    mx_printf("ishader: the batch ran in the buffer at %u, not %u\n", top, buf * s->buf_qw);
    return -1;
  }
  mx_vu1_read_dma(out, s->out_at, 1 + s->out_qw * ni, (void *)0x70000000, 16384);
  return 0;
}

static int bad;
static void check(const char *name, const struct mx_shader *s, u32 nv, u32 ni) {
  CHECK_EQ(out[0][0] & 0x7FFF, ni);
  CHECK_EQ(out[0][0] >> 15 & 1, 1);
  u32 o = s->out_qw;
  int before = bad;
  for (u32 k = 0; k < ni; k++)
    for (u32 r = 0; r < o; r++)
      for (u32 f = 0; f < 4; f++) {
        u32 got = out[1 + k * o + r][f], want = tab[1 + idx[k] + r][f];
        if (got != want && bad++ < 8)
          mx_printf("%s nv %u ni %u index %u (vertex %u) reg %u[%u]: %08x, expected %08x\n", name,
                    nv, ni, k, idx[k] / o, r, f, got, want);
      }
  if (bad != before)
    t_fail++;
}

/* ---- the EE version of mskin ------------------------------------------------- */

static float fmaxf_(float a, float b) { return a > b ? a : b; }
static float fminf_(float a, float b) { return a < b ? a : b; }
static float fabsf_(float a) { return a < 0 ? -a : a; }
static float rsqrtf_(float v) { return 1.0f / __builtin_sqrtf(v); }

static int ref_bad;
static const char *ref_name; /* the program the EE version is of */
static void near_int(u32 i, const char *what, int k, u32 got, s32 want, s32 tol) {
  s32 d = (s32)got - want;
  if ((d > tol || -d > tol) && ref_bad++ < 6)
    mx_printf("%s vertex %u: %s[%d] = %d, expected %d\n", ref_name, i, what, k, (s32)got, want);
}
static void near_float(u32 i, const char *what, int k, u32 got, float want) {
  union {
    u32 u;
    float f;
  } g = {got};
  if (fabsf_(g.f - want) > 2e-4f * fabsf_(want) + 1e-6f && ref_bad++ < 6)
    mx_printf("mskin vertex %u: %s[%d] = %d/100000, expected %d/100000\n", i, what, k,
              (s32)(g.f * 100000.0f), (s32)(want * 100000.0f));
}

/* The same expressions as apps/model/mskin.hlsl; the uniforms in the places
   of its header (mvp 0-3). */
static float wrap(float nl) {
  float h = nl * 0.5f + 0.5f;
  return h * h;
}
static float spec(const float *n, const float *l, const float *e) {
  float h[3] = {l[0] + e[0], l[1] + e[1], l[2] + e[2]};
  float s = fmaxf_((n[0] * h[0] + n[1] * h[1] + n[2] * h[2]) *
                       rsqrtf_(h[0] * h[0] + h[1] * h[1] + h[2] * h[2] + 1e-12f),
                   0);
  s = s * s;
  s = s * s;
  s = s * s;
  return s * s;
}
static void ref_skin(u32 n) {
  const float *light = uni[VU_MSKIN_LIGHT], *lcol = uni[VU_MSKIN_LCOL], *amb = uni[VU_MSKIN_AMB];
  const float *eye = uni[VU_MSKIN_EYE];
  for (u32 i = 0; i < n; i++) {
    float c[4][4] = {{0}};
    for (int k = 0; k < 4; k++) {
      float w = (float)(s32)wgt[i][k] / 255.0f;
      for (int m = 0; m < 4; m++)
        for (int f = 0; f < 4; f++)
          c[m][f] += uni[VU_MSKIN_JOINT + 4 * jnt[i][k] + m][f] * w;
    }
    float v[3] = {pos16[i][0] / 32768.0f, pos16[i][1] / 32768.0f, pos16[i][2] / 32768.0f};
    float mp[4], nv[3], p[4];
    for (int f = 0; f < 4; f++)
      mp[f] = c[0][f] * v[0] + c[1][f] * v[1] + c[2][f] * v[2] + c[3][f];
    for (int f = 0; f < 3; f++)
      nv[f] = c[0][f] * nrm[i][0] + c[1][f] * nrm[i][1] + c[2][f] * nrm[i][2];
    for (int f = 0; f < 4; f++)
      p[f] = uni[0][f] * mp[0] + uni[1][f] * mp[1] + uni[2][f] * mp[2] + uni[3][f];
    float q = 1.0f / p[3];
    float r = rsqrtf_(nv[0] * nv[0] + nv[1] * nv[1] + nv[2] * nv[2] + 1e-6f);
    for (int f = 0; f < 3; f++)
      nv[f] *= r;
    float d = wrap(nv[0] * light[0] + nv[1] * light[1] + nv[2] * light[2]);
    float e[3] = {eye[0] - mp[0], eye[1] - mp[1], eye[2] - mp[2]};
    float er = rsqrtf_(e[0] * e[0] + e[1] * e[1] + e[2] * e[2] + 1e-12f);
    for (int f = 0; f < 3; f++)
      e[f] *= er;
    float s = eye[3] * spec(nv, light, e);
    u32(*o)[4] = &tab[1 + 3 * i];
    near_float(i, "st", 0, o[0][0], (float)uv[i][0] / 4096.0f * q);
    near_float(i, "st", 1, o[0][1], (float)uv[i][1] / 4096.0f * q);
    near_float(i, "st", 2, o[0][2], q);
    for (int f = 0; f < 3; f++)
      near_int(i, "rgbaq", f, o[1][f], (s32)fminf_(amb[f] + lcol[f] * d, 255), 1);
    near_int(i, "rgbaq", 3, o[1][3], (s32)fminf_(s, 255), 1);
    for (int f = 0; f < 3; f++)
      near_int(i, "xyz2", f, o[2][f], (s32)(p[f] * q * 16.0f), 2);
  }
  if (ref_bad)
    t_fail++;
}

/* ---- uniforms ------------------------------------------------------------------- */

_Static_assert(VU_MLIT_EYE == VU_MTEX_EYE && VU_MLIT_EYE == VU_MSKIN_EYE, "");

static void set_uniforms(float (*u)[4], const struct prog *p, float scale) {
  static const float m[4][4] = {
      {100, 0, 0, 0}, {0, 80, 0, 0}, {0, 0, 1000, 0.25f}, {3072, 3072, 3000, 1.5f}};
  for (int i = 0; i < NUNI; i++)
    for (int k = 0; k < 4; k++)
      u[i][k] = i < 4 ? m[i][k] * scale : 0;
  static const float L[3] = {0.2575f, -0.4292f, -0.8584f};
  if (p->model && !p->lit) {
    /* mflat: the colour */
    u[4][0] = 200 * scale, u[4][1] = 100, u[4][2] = 50, u[4][3] = 128;
  } else if (p->lit) {
    for (int k = 0; k < 3; k++)
      u[4][k] = L[k] / (p->model == 2 ? 1.0f : 127.0f) * scale;
    if (p->model) {
      u[5][0] = 100 * scale, u[5][1] = 90, u[5][2] = 80;
      u[6][0] = u[6][1] = u[6][2] = 30 * scale, u[6][3] = 128;
    } else {
      u[5][0] = 0.9f, u[5][1] = 0.8f, u[5][2] = 0.7f;
      u[6][0] = u[6][1] = u[6][2] = 0.2f * scale, u[6][3] = 1;
    }
  }
  if (p->lit && p->model) {
    /* the eye (in front, where the highlights are) and the highlight's strength */
    u[VU_MLIT_EYE][0] = 0.5f, u[VU_MLIT_EYE][1] = -0.8f * scale, u[VU_MLIT_EYE][2] = -2;
    u[VU_MLIT_EYE][3] = 200;
  }
  if (p->model == 2)
    /* mskin: the joints, rotations and moves around 1 */
    for (int j = 0; j < NJOINT; j++)
      for (int c = 0; c < 4; c++)
        for (int f = 0; f < 4; f++)
          u[VU_MSKIN_JOINT + 4 * j + c][f] = f == 3 ? (c == 3 ? 1.0f : 0.0f)
                                       : (c == f ? 1.0f : 0.0f) + 0.3f * rnd() * scale;
}

int main(void) {
  mx_gs_init(640, 224);
  mx_vif_reset(1);
  prim = (u32)mx_gs_prim(MX_GS_PRIM_TRISTRIP, 1, 0, 0, 0, 0, 0, 0, 0);
  struct prog pr[] = {
      {"lvl0", MX_VU_SHADER(LVL0, lvl0_vu, lvl0_vu_end),
       MX_VU_ISHADER(IX_LVL0, lvl0_ix_vu, lvl0_ix_vu_end), {POS, COL}, 2, 0, 0, 0, 0},
      {"lvl1", MX_VU_SHADER(LVL1, lvl1_vu, lvl1_vu_end),
       MX_VU_ISHADER(IX_LVL1, lvl1_ix_vu, lvl1_ix_vu_end), {POS, NRM, COL}, 3, 1, 0, 0, 0},
      {"lvl2", MX_VU_SHADER(LVL2, lvl2_vu, lvl2_vu_end),
       MX_VU_ISHADER(IX_LVL2, lvl2_ix_vu, lvl2_ix_vu_end), {POS, NRM, COL, UV}, 4, 1, 1, 0, 0},
      {"mflat", MX_VU_SHADER(MFLAT, mflat_vu, mflat_vu_end),
       MX_VU_ISHADER(IX_MFLAT, mflat_ix_vu, mflat_ix_vu_end), {POS}, 1, 0, 0, 1, 0},
      {"mlit", MX_VU_SHADER(MLIT, mlit_vu, mlit_vu_end),
       MX_VU_ISHADER(IX_MLIT, mlit_ix_vu, mlit_ix_vu_end), {POS, NRM}, 2, 1, 0, 1, 0},
      {"mtex", MX_VU_SHADER(MTEX, mtex_vu, mtex_vu_end),
       MX_VU_ISHADER(IX_MTEX, mtex_ix_vu, mtex_ix_vu_end), {POS, NRM, UV}, 3, 1, 1, 1, 0},
      {"mskin", MX_VU_SHADER(MSKIN, mskin_vu, mskin_vu_end),
       MX_VU_ISHADER(IX_MSKIN, mskin_ix_vu, mskin_ix_vu_end), {POS16, NRM, UV, JNT, WGT}, 5, 1,
       1, 2, ref_skin},
  };
  const u32 nprog = sizeof pr / sizeof pr[0];
  for (u32 i = 0; i < NV + 16; i++) {
    pos[i][0] = rnd(), pos[i][1] = rnd(), pos[i][2] = rnd();
    for (int k = 0; k < 3; k++)
      pos16[i][k] = (s16)(s32)(pos[i][k] * 16384.0f);
    float a = rnd(), b = rnd(), c = rnd(), l = a * a + b * b + c * c + 1e-3f, r = 1;
    for (int k = 0; k < 8; k++)
      r = r * (1.5f - 0.5f * l * r * r);
    nrm[i][0] = (s8)(s32)(a * r * 127), nrm[i][1] = (s8)(s32)(b * r * 127);
    nrm[i][2] = (s8)(s32)(c * r * 127), nrm[i][3] = 0;
    col[i] = rnd32();
    uv[i][0] = (s16)(rnd32() & 0x1FFF), uv[i][1] = (s16)(rnd32() & 0x1FFF);
    /* four joints, weights summing to 255 */
    u32 left = 255;
    for (int k = 0; k < 4; k++) {
      jnt[i][k] = (u8)((rnd32() >> 8) % NJOINT);
      u32 w = k == 3 ? left : (rnd32() >> 7) % (left + 1);
      wgt[i][k] = (u8)w;
      left -= w;
    }
  }
  mx_dcache_writeback(pos, sizeof pos);
  mx_dcache_writeback(pos16, sizeof pos16);
  mx_dcache_writeback(nrm, sizeof nrm);
  mx_dcache_writeback(col, sizeof col);
  mx_dcache_writeback(uv, sizeof uv);
  mx_dcache_writeback(jnt, sizeof jnt);
  mx_dcache_writeback(wgt, sizeof wgt);

  u32 runs = 0;
  for (u32 l = 0; l < nprog; l++) {
    struct prog *p = &pr[l];
    int fails = t_fail;
    CHECK(p->ix.max_i >= 16 && p->ix.max_n <= NV && p->ix.max_i <= NI);
    CHECK(p->strip.uni_qw <= NUNI && p->ix.uni_qw == p->strip.uni_qw);
    CHECK_EQ(p->ix.out_qw, p->strip.out_qw);
    CHECK_EQ(p->ix.nin, (u32)p->nin);
    set_uniforms(uni, p, 1.0f);
    /* the other block: lit differently and moved (a scale alone would give
       the same P / P.w) */
    set_uniforms(other, p, 0.5f);
    other[3][0] += 400, other[3][1] -= 300;
    for (u32 nv = 1; nv <= p->ix.max_n; nv++) {
      /* the outputs of the vertices by the strip program */
      mx_vu_reset(2);
      mx_vif_reset(1);
      mx_vu_forget(1);
      mx_vu_load(1, p->strip.code, p->strip.code_bytes, p->strip.entry);
      if (run_strip(p, nv)) {
        mx_printf("%s nv %u: the strip batch did not end\n", p->name, nv);
        t_fail++;
        break;
      }
      if (p->ref)
        p->ref(nv);
      mx_vu_reset(2);
      mx_vif_reset(1);
      mx_vu_forget(1);
      mx_vu_load(1, p->ix.code, p->ix.code_bytes, p->ix.entry);
      /* index counts: all of them for the smallest and largest batch, and
         those around the multiples of 4 and 16 for the others */
      for (u32 ni = 1; ni <= p->ix.max_i; ni++) {
        u32 m = ni % 16;
        if (nv != 1 && nv != p->ix.max_n && ni > 6 && m != 0 && m != 1 && m != 15 &&
            ni % 4 != 3 && ni != p->ix.max_i)
          continue;
        for (u32 k = 0; k < ni; k++)
          idx[k] = (u8)(rnd32() % nv * p->ix.out_qw);
        /* the room after the last index: not used for the packet */
        for (u32 k = ni; k < (ni + 15) / 16 * 16; k++)
          idx[k] = 0xFF;
        u32 blk = runs & 1, buf = runs >> 1 & 1;
        runs++;
        if (run_indexed(p, nv, ni, blk, buf)) {
          mx_printf("%s nv %u ni %u: the indexed batch did not end\n", p->name, nv, ni);
          t_fail++;
          goto next;
        }
        check(p->name, &p->ix, nv, ni);
        if (bad > 8)
          goto next;
      }
    }
  next:
    mx_printf("%s_ix: %u vertices, %u indices per batch, %s\n", p->name, p->ix.max_n, p->ix.max_i,
              t_fail == fails ? "ok" : "FAILED");
  }
  mx_printf("ishader: %u indexed batches\n", runs);
  return t_done("ishader");
}
