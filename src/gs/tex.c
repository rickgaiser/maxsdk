#include <mx/bits.h>
#include <mx/tex.h>

void mx_tex_page_size(u32 psm, u32 *pw, u32 *ph) {
  *pw = 64, *ph = 32; /* 32/24-bit, and T8H/T4HL/T4HH (stored in 32-bit pixels) */
  switch (psm) {
  case MX_GS_PSM_CT16:
  case MX_GS_PSM_CT16S:
  case MX_GS_PSM_Z16:
  case MX_GS_PSM_Z16S:
    *ph = 64;
    break;
  case MX_GS_PSM_T8:
    *pw = 128, *ph = 64;
    break;
  case MX_GS_PSM_T4:
    *pw = 128, *ph = 128;
    break;
  }
}

u32 mx_tex_tbw(u32 w, u32 psm) {
  u32 pw, ph;
  mx_tex_page_size(psm, &pw, &ph);
  return (w < pw ? pw : w) / 64;
}

/* The block order in a page (GS manual 8.3): the block at block row r,
   column c. 32-bit and T8 pages are 8 x 4 blocks, 16-bit and T4 4 x 8. */
static const u8 order32[4][8] = {{0, 1, 4, 5, 16, 17, 20, 21},
                                 {2, 3, 6, 7, 18, 19, 22, 23},
                                 {8, 9, 12, 13, 24, 25, 28, 29},
                                 {10, 11, 14, 15, 26, 27, 30, 31}};
static const u8 order16[8][4] = {{0, 2, 8, 10},    {1, 3, 9, 11},    {4, 6, 12, 14},
                                 {5, 7, 13, 15},   {16, 18, 24, 26}, {17, 19, 25, 27},
                                 {20, 22, 28, 30}, {21, 23, 29, 31}};
static const u8 order16s[8][4] = {{0, 2, 16, 18},   {1, 3, 17, 19},  {8, 10, 24, 26},
                                  {9, 11, 25, 27},  {4, 6, 20, 22},  {5, 7, 21, 23},
                                  {12, 14, 28, 30}, {13, 15, 29, 31}};

u32 mx_tex_blocks(u32 w, u32 h, u32 psm) {
  u32 pw, ph;
  mx_tex_page_size(psm, &pw, &ph);
  const u8 *order;
  u32 cols;
  switch (psm) {
  case MX_GS_PSM_CT32:
  case MX_GS_PSM_CT24:
  case MX_GS_PSM_T8:
  case MX_GS_PSM_T8H:
  case MX_GS_PSM_T4HL:
  case MX_GS_PSM_T4HH:
    order = &order32[0][0], cols = 8;
    break;
  case MX_GS_PSM_CT16:
  case MX_GS_PSM_T4:
    order = &order16[0][0], cols = 4;
    break;
  case MX_GS_PSM_CT16S:
    order = &order16s[0][0], cols = 4;
    break;
  default: /* Z formats: whole pages */
    order = 0, cols = 0;
    break;
  }
  if (!order || w > pw || h > ph)
    return (w + pw - 1) / pw * ((h + ph - 1) / ph) * 32;
  u32 bw = pw / cols, bh = ph / (32 / cols), last = 0;
  for (u32 r = 0; r < (h + bh - 1) / bh; r++)
    for (u32 c = 0; c < (w + bw - 1) / bw; c++)
      if (order[r * cols + c] > last)
        last = order[r * cols + c];
  return last + 1;
}

u32 mx_tex_max_levels(u32 tw, u32 th, u32 min) {
  u32 m = 0, s = tw < th ? tw : th;
  while ((1u << (m + 1)) <= min)
    m++;
  u32 n = s > m ? s - m + 1 : 1;
  return n > MX_TEX_LEVELS ? MX_TEX_LEVELS : n;
}

u32 mx_tex_init(struct mx_tex *t, u32 tbp, u32 psm, u32 tw, u32 th, u32 levels, int layout) {
  *t = (struct mx_tex){.psm = (u8)psm, .tw = (u8)tw, .th = (u8)th, .levels = (u8)levels};
  if (levels < 1)
    t->levels = 1;
  if (levels > MX_TEX_LEVELS)
    t->levels = MX_TEX_LEVELS;
  u32 at = tbp;
  for (u32 n = 0; n < t->levels; n++) {
    u32 w = 1u << (tw > n ? tw - n : 0), h = 1u << (th > n ? th - n : 0);
    u32 b = mx_tex_blocks(w, h, psm);
    if (layout == MX_TEX_PAGES || b >= 32)
      b = (b + 31) & ~31u;
    u32 align = 1; /* a page, or the power of two that holds b */
    while (align < b && align < 32)
      align <<= 1;
    at = (at + align - 1) & ~(align - 1);
    t->tbp[n] = at;
    t->tbw[n] = (u8)mx_tex_tbw(w, psm);
    at += b;
  }
  t->cpsm = MX_GS_PSM_CT32;
  return t->blocks = at - tbp;
}

u64 mx_tex_tex0(const struct mx_tex *t, u32 tcc, u32 tfx) {
  u64 v = mx_gs_tex0(t->tbp[0], t->tbw[0], t->psm, t->tw, t->th, tcc, tfx);
  if (t->psm == MX_GS_PSM_T8 || t->psm == MX_GS_PSM_T4 || t->psm == MX_GS_PSM_T8H ||
      t->psm == MX_GS_PSM_T4HL || t->psm == MX_GS_PSM_T4HH)
    v |= MX_SET(GS_TEX0_CBP, t->cbp) | MX_SET(GS_TEX0_CPSM, t->cpsm) | MX_SET(GS_TEX0_CSM, t->csm) |
         MX_SET(GS_TEX0_CLD, 1);
  return v;
}

/* levels n .. n + 2 */
static u64 miptbp(const struct mx_tex *t, u32 n) {
  u32 p[3] = {0, 0, 0}, w[3] = {0, 0, 0};
  for (u32 i = 0; i < 3; i++)
    if (n + i < t->levels)
      p[i] = t->tbp[n + i], w[i] = t->tbw[n + i];
  return mx_gs_miptbp(p[0], w[0], p[1], w[1], p[2], w[2]);
}

u64 mx_tex_miptbp1(const struct mx_tex *t) { return miptbp(t, 1); }
u64 mx_tex_miptbp2(const struct mx_tex *t) { return miptbp(t, 4); }

u64 mx_tex_tex1(const struct mx_tex *t, u32 mmag, u32 mmin, u32 l, s32 k) {
  return mx_gs_tex1(0, t->levels - 1u, mmag, mmin, 0, l, (u32)k);
}

u64 *mx_tex_e_regs(u64 *c, const struct mx_tex *t, int ctx, u64 tex0, u64 tex1) {
  c = mx_e_ad(c, mx_gs_ctx(MX_GS_REG_TEX1_1, ctx), tex1);
  c = mx_e_ad(c, mx_gs_ctx(MX_GS_REG_TEX0_1, ctx), tex0);
  c = mx_e_ad(c, mx_gs_ctx(MX_GS_REG_MIPTBP1_1, ctx), mx_tex_miptbp1(t));
  return mx_e_ad(c, mx_gs_ctx(MX_GS_REG_MIPTBP2_1, ctx), mx_tex_miptbp2(t));
}

/* log2(x) for x > 0 in 1/256: the exponent, then the mantissa's bits by
   squaring. The bits are summed as an integer: InstCombine turns a
   conditional float add into an add of a select without fast-math flags,
   which is __addsf3 on the R5900 (docs/llvm-issues.md 26). */
static s32 log2_256(float x) {
  union {
    float f;
    u32 u;
  } v = {x};
  s32 r = ((s32)(v.u >> 23 & 0xFF) - 127) * 256;
  v.u = (v.u & 0x7FFFFF) | 0x3F800000u; /* 1 <= m < 2 */
  float m = v.f;
  for (s32 bit = 128; bit; bit >>= 1) {
    m *= m;
    if (m >= 2.0f)
      m *= 0.5f, r += bit;
  }
  return r;
}

s32 mx_tex_lod_k(float texels_per_unit, float pixels_per_unit, u32 l) {
  if (!(texels_per_unit > 0.0f) || !(pixels_per_unit > 0.0f))
    return 0;
  /* 1/256 to 1/16 << l, rounded to nearest */
  s32 d = (log2_256(texels_per_unit) - log2_256(pixels_per_unit)) << l;
  s32 k = d >= 0 ? (d + 8) >> 4 : -((-d + 8) >> 4);
  return k < -2048 ? -2048 : k > 2047 ? 2047 : k;
}

void mx_tex_rt(const struct mx_tex *t, u32 level, struct mx_rt *rt) {
  mx_rt_init(rt, t->tbp[level] / 32, 1u << (t->tw - level), 1u << (t->th - level), t->psm, 0,
             MX_RT_NOZ);
}

u64 *mx_tex_e_mipgen(u64 *c, const struct mx_tex *t, int ctx, int eop) {
  for (u32 n = 1; n < t->levels; n++) {
    struct mx_rt rt;
    mx_tex_rt(t, n, &rt);
    u32 w = rt.w, h = rt.h;
    c = mx_e_ad_tag(c, 14, eop && n == t->levels - 1u);
    c = mx_rt_e_regs(c, &rt, ctx);
    c = mx_e_ad(c, mx_gs_ctx(MX_GS_REG_TEST_1, ctx), mx_gs_test(0, 0, 0, 0, 0, 0, 1, 1));
    c = mx_e_ad(
        c, mx_gs_ctx(MX_GS_REG_TEX0_1, ctx),
        mx_gs_tex0(t->tbp[n - 1], t->tbw[n - 1], t->psm, t->tw - (n - 1), t->th - (n - 1), 1, 1));
    c = mx_e_ad(c, mx_gs_ctx(MX_GS_REG_TEX1_1, ctx), mx_gs_tex1(1, 0, 1, 1, 0, 0, 0));
    c = mx_e_ad(c, mx_gs_ctx(MX_GS_REG_CLAMP_1, ctx), mx_gs_clamp(1, 1, 0, 0, 0, 0));
    c = mx_e_ad(c, MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 1, 0, 0, 0, 1, ctx, 0));
    /* pixel x samples u = 2x + 1: between the centres of texels 2x and
       2x + 1 (texel j's centre is j + 0.5), so bilinear takes their mean */
    c = mx_e_ad(c, MX_GS_REG_UV, mx_gs_uv(1 << 4, 1 << 4));
    c = mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(mx_rt_px(&rt, 0), mx_rt_py(&rt, 0), 0));
    c = mx_e_ad(c, MX_GS_REG_UV, mx_gs_uv((2 * w + 1) << 4, (2 * h + 1) << 4));
    c = mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(mx_rt_px(&rt, w), mx_rt_py(&rt, h), 0));
    c = mx_e_ad(c, MX_GS_REG_TEXFLUSH, 0);
  }
  return c;
}
