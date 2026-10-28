/* MIPMAP textures (<mx/tex.h>): the level layout, MIPTBP, the LOD and the
   levels the GS makes itself.

   For CT32, CT16, T8 and T4 (with a CLUT) a 64 x 64 texture of 4 levels
   (64, 32, 16, 8), packed, each level one colour (T8/T4: the CLUT entry
   level + 1). Each frame:
   (a) LCM 1, LOD = K = n: a sprite per level shows level n's colour (the
       layout, MIPTBP and the block order: an overlap, a wrong base or a
       wrong block table gives another colour);
   (b) LCM 0, K 0, STQ: sprites with Q = 1, 1/4, 1/8 show levels 0, 2, 3
       (LOD = log2(1/Q));
   (c) mx_tex_e_mipgen in context 2: a 16 x 16 CT32 texture of single-texel
       checks of two colours, its 8 x 8 level made by the GS: every texel
       the mean of the two.
   Run in ring and in chain mode. */

#include <mx/tex.h>
#include "t.h"

#define FRAMES 4
#define NBUF 2
#define NFMT 4
#define LV 4

static u8 ring[64 * 1024] MX_ALIGNED(64 * 1024);
static u8 slots[64 * 1024] MX_ALIGNED(64);
static u32 px[32] MX_ALIGNED(64);

static struct mx_rt fb[NBUF];
static struct mx_display disp[NBUF];

static const u32 psms[NFMT] = {MX_GS_PSM_CT32, MX_GS_PSM_CT16, MX_GS_PSM_T8, MX_GS_PSM_T4};
static const char *const names[NFMT] = {"CT32", "CT16", "T8", "T4"};
static struct mx_tex tex[NFMT], gen;

#define RGB(r, g, b) ((u32)(r) | (u32)(g) << 8 | (u32)(b) << 16)
/* level colours, exact in 5 bits a channel (CT16) */
static const u32 col[LV] = {RGB(0xF8, 0x00, 0x00), RGB(0x00, 0xF8, 0x00), RGB(0x00, 0x00, 0xF8),
                            RGB(0xF8, 0xF8, 0x00)};
static const u32 CA = RGB(0x40, 0x80, 0xC0), CB = RGB(0xC0, 0x00, 0x40);

/* the level data: 64 x 64 CT32 at most, each level from 16 bytes */
static u8 data[NFMT][LV][64 * 64 * 4] MX_ALIGNED(64);
static u32 clut[256] MX_ALIGNED(64);
static u32 checker[16 * 16] MX_ALIGNED(64);

static u16 ct16(u32 c) {
  return (u16)((c >> 3 & 0x1F) | (c >> 11 & 0x1F) << 5 | (c >> 19 & 0x1F) << 10 | 0x8000);
}

static void fill(int f, u32 n) {
  u32 w = 64u >> n, cnt = w * w;
  u8 *d = data[f][n];
  for (u32 i = 0; i < cnt; i++)
    switch (psms[f]) {
    case MX_GS_PSM_CT32:
      ((u32 *)d)[i] = col[n] | 0x80000000u;
      break;
    case MX_GS_PSM_CT16:
      ((u16 *)d)[i] = ct16(col[n]);
      break;
    case MX_GS_PSM_T8:
      d[i] = (u8)(n + 1);
      break;
    case MX_GS_PSM_T4:
      d[i / 2] = (u8)((n + 1) | (n + 1) << 4);
      break;
    }
}

static void upload(void) {
  /* the textures from mx_gs.tbp, then the CLUTs (a 16 x 16 and an 8 x 2
     CT32 CLUT: 4 blocks each are enough) */
  u32 at = mx_gs.tbp;
  for (int f = 0; f < NFMT; f++) {
    at += mx_tex_init(&tex[f], at, psms[f], 6, 6, LV, MX_TEX_PACKED);
    at = (at + 31) & ~31u;
    const void *lv[LV];
    for (u32 n = 0; n < LV; n++) {
      fill(f, n);
      lv[n] = data[f][n];
    }
    mx_dcache_writeback(data[f], sizeof data[f]);
    CHECK_EQ(mx_frame_tex_upload(0, &tex[f], lv), 0);
    CHECK_EQ(mx_dma_wait(MX_CH_GIF), 0);
    mx_printf("mip: %s levels at %u %u %u %u, %u blocks\n", names[f], tex[f].tbp[0], tex[f].tbp[1],
              tex[f].tbp[2], tex[f].tbp[3], tex[f].blocks);
  }
  for (int i = 0; i < 256; i++)
    clut[i] = i >= 1 && i <= LV ? col[i - 1] | 0x80000000u : 0x80000000u;
  mx_dcache_writeback(clut, sizeof clut);
  tex[2].cbp = at;
  tex[3].cbp = at + 4;
  CHECK_EQ(mx_frame_texture(0, tex[2].cbp, 1, MX_GS_PSM_CT32, 16, 16, clut), 0);
  CHECK_EQ(mx_dma_wait(MX_CH_GIF), 0);
  CHECK_EQ(mx_frame_texture(0, tex[3].cbp, 1, MX_GS_PSM_CT32, 8, 2, clut), 0);
  CHECK_EQ(mx_dma_wait(MX_CH_GIF), 0);

  /* (c): level 0 single-texel checks; level 1 left to the GS */
  for (int y = 0; y < 16; y++)
    for (int x = 0; x < 16; x++)
      checker[y * 16 + x] = ((x ^ y) & 1 ? CB : CA) | 0x80000000u;
  mx_dcache_writeback(checker, sizeof checker);
  CHECK_EQ(mx_frame_texture(0, gen.tbp[0], gen.tbw[0], MX_GS_PSM_CT32, 16, 16, checker), 0);
  CHECK_EQ(mx_dma_wait(MX_CH_GIF), 0);
}

/* a 16 x 16 sprite at window (x, y) of f */
static void sprite_uv(const struct mx_rt *f, int f_i, u32 n, u32 x, u32 y) {
  const struct mx_tex *t = &tex[f_i];
  u64 *c = mx_frame_direct(10);
  c = mx_e_ad_tag(c, 9, 1);
  c = mx_tex_e_regs(c, t, 0, mx_tex_tex0(t, 1, 1), mx_gs_tex1(1, LV - 1, 0, 2, 0, 0, n * 16));
  c = mx_e_ad(c, MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 1, 0, 0, 0, 1, 0, 0));
  c = mx_e_ad(c, MX_GS_REG_UV, mx_gs_uv(0, 0));
  c = mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(mx_rt_px(f, x), mx_rt_py(f, y), 0));
  c = mx_e_ad(c, MX_GS_REG_UV, mx_gs_uv(64 << 4, 64 << 4));
  mx_frame_commit(
      mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(mx_rt_px(f, x + 16), mx_rt_py(f, y + 16), 0)));
}

/* the same with STQ and Q = q: LOD = log2(1/q) */
static void sprite_q(const struct mx_rt *f, int f_i, float q, u32 x, u32 y) {
  const struct mx_tex *t = &tex[f_i];
  u64 *c = mx_frame_direct(11);
  c = mx_e_ad_tag(c, 10, 1);
  c = mx_tex_e_regs(c, t, 0, mx_tex_tex0(t, 1, 1), mx_tex_tex1(t, 0, 2, 0, 0));
  c = mx_e_ad(c, MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 1, 0, 0, 0, 0, 0, 0));
  c = mx_e_ad(c, MX_GS_REG_RGBAQ, mx_gs_rgbaq(0x80, 0x80, 0x80, 0x80, q));
  c = mx_e_ad(c, MX_GS_REG_ST, mx_gs_st(0.0f, 0.0f));
  c = mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(mx_rt_px(f, x), mx_rt_py(f, y), 0));
  c = mx_e_ad(c, MX_GS_REG_ST, mx_gs_st(q, q));
  mx_frame_commit(
      mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(mx_rt_px(f, x + 16), mx_rt_py(f, y + 16), 0)));
}

static const float qs[3] = {1.0f, 0.25f, 0.125f};
static const u32 q_level[3] = {0, 2, 3};

static void frame(void) {
  u32 k = mx_frame_begin_n();
  const struct mx_rt *f = &fb[k % NBUF];
  mx_frame_target(f, 0);
  u64 *c = mx_frame_direct(5);
  c = mx_e_ad_tag(c, 4, 1);
  c = mx_e_ad(c, MX_GS_REG_TEST_1, mx_gs_test(0, 0, 0, 0, 0, 0, 1, 1));
  c = mx_e_ad(c, MX_GS_REG_PRMODECONT, 1);
  c = mx_e_ad(c, MX_GS_REG_CLAMP_1, mx_gs_clamp(0, 0, 0, 0, 0, 0));
  mx_frame_commit(mx_e_ad(c, MX_GS_REG_TEXA, 0x80ull << 32));
  c = mx_frame_direct(5);
  mx_frame_commit(mx_rt_e_clear(c, f, 0, 0x10, 0x20, 0x30, 1));

  for (int i = 0; i < NFMT; i++) {
    for (u32 n = 0; n < LV; n++)
      sprite_uv(f, i, n, 40 + 80 * (u32)i, 20 + 20 * n);
    for (u32 j = 0; j < 3; j++)
      sprite_q(f, i, qs[j], 40 + 80 * (u32)i + 30, 20 + 20 * j);
  }

  /* (c) in context 2 */
  c = mx_frame_direct(MX_TEX_MIPGEN_QW(2));
  mx_frame_commit(mx_tex_e_mipgen(c, &gen, 1, 1));
  mx_frame_end();
}

static u32 pixel(u32 bp, u32 bw, u32 x, u32 y) {
  mx_gs_download(px, bp, bw, MX_GS_PSM_CT32, x & ~31u, y, 32, 1);
  return px[x & 31] & 0xFFFFFF;
}

static int near(u32 a, u32 b) {
  for (int s = 0; s < 24; s += 8) {
    int d = (int)(a >> s & 0xFF) - (int)(b >> s & 0xFF);
    if (d < -1 || d > 1)
      return 0;
  }
  return 1;
}

static void run(int chain) {
  if (chain)
    CHECK_EQ(mx_frame_init_chain(slots, sizeof slots, (void *)0x70002000, 0x2000), 0);
  else
    mx_frame_init(ring, sizeof ring, (void *)0x70002000, 0x2000);
  for (int i = 0; i < FRAMES; i++)
    frame();
  u32 last = mx_gs.frames - 1, t0 = mx_count();
  while (mx_gs.shown < last + 1 && mx_count() - t0 < 300000000u)
    ;
  mx_frame_stop();
  CHECK_EQ(mx_gs.shown, last + 1);

  const struct mx_rt *f = &fb[last % NBUF];
  u32 bp = MX_GS_PAGE(f->fbp);
  for (int i = 0; i < NFMT; i++) {
    for (u32 n = 0; n < LV; n++) {
      u32 v = pixel(bp, f->fbw, 40 + 80 * (u32)i + 8, 20 + 20 * n + 8);
      if (v != col[n])
        mx_printf("mip: %s K %u: 0x%06x, expected level %u (0x%06x)\n", names[i], n, v, n, col[n]);
      CHECK_EQ(v, col[n]);
    }
    for (u32 j = 0; j < 3; j++) {
      u32 v = pixel(bp, f->fbw, 40 + 80 * (u32)i + 38, 20 + 20 * j + 8);
      if (v != col[q_level[j]])
        mx_printf("mip: %s Q 1/%u: 0x%06x, expected level %u (0x%06x)\n", names[i],
                  (u32)(1.0f / qs[j]), v, q_level[j], col[q_level[j]]);
      CHECK_EQ(v, col[q_level[j]]);
    }
  }
  /* (c) the GS's level 1: the mean of the two colours */
  const u32 mean = RGB(0x80, 0x40, 0x80);
  static const u8 at[4][2] = {{0, 0}, {3, 5}, {7, 7}, {6, 1}};
  for (int i = 0; i < 4; i++) {
    u32 v = pixel(gen.tbp[1], gen.tbw[1], at[i][0], at[i][1]);
    if (!near(v, mean))
      mx_printf("mip: mipgen texel (%u, %u): 0x%06x, expected 0x%06x\n", at[i][0], at[i][1], v,
                mean);
    CHECK(near(v, mean));
  }
  mx_printf("mip: %s mode, frames to %u, missed %u\n", chain ? "chain" : "ring", last,
            mx_gs.missed);
}

int main(void) {
  struct mx_video_mode m = mx_video_ntsc;
  m.ffmd = 1;
  mx_gs_init_video(&m);
  u32 pages = mx_rt_pages(640, 224, MX_GS_PSM_CT32);
  for (int i = 0; i < NBUF; i++) {
    mx_rt_init(&fb[i], mx_vram_alloc(pages), 640, 224, MX_GS_PSM_CT32, 0, MX_RT_NOZ);
    mx_display_init(&disp[i], &fb[i]);
  }
  /* (c): a level a page */
  u32 gp = mx_vram_alloc(2);
  CHECK_EQ(mx_tex_init(&gen, MX_GS_PAGE(gp), MX_GS_PSM_CT32, 4, 4, 2, MX_TEX_PAGES), 64);
  mx_gs_displays(disp, NBUF);
  mx_display_apply(&disp[0]);

  upload();
  run(0);
  run(1);
  return t_done("mip");
}
