/* Tutorial 2, step 2: textures. A texture is made in main memory, sent to
   GS memory (mx_frame_texture: by DMA over PATH3, the GIF's own channel),
   and drawn on sprites and triangles. TEX0 says where it is and how it is
   applied, TEX1 how it is filtered; UV gives each vertex its texel (12.4
   fixed point, like XYZ2). */

#include "tut.h"

#define FRAMES 120
#define TW 64 /* the texture: 64 x 64, 32-bit colour */

static u8 ring[16 * 1024] MX_ALIGNED(16 * 1024);
/* the DMA reads the texture from memory: 64-byte aligned, out of the cache */
static u32 tex[TW * TW] MX_ALIGNED(64);

/* the texture: a checkerboard of 4 x 4 texels in two shades over a
   colour gradient. A texel is 0xAABBGGRR; alpha 0x80 is opaque on the GS. */
static void make_texture(void) {
  for (u32 y = 0; y < TW; y++)
    for (u32 x = 0; x < TW; x++) {
      u32 k = ((x ^ y) & 4) ? 0xFF : 0x80;
      u32 r = k * x / TW, g = k * y / TW, b = k * (2 * TW - 1 - x - y) / (2 * TW);
      tex[y * TW + x] = 0x80u << 24 | b << 16 | g << 8 | r;
    }
  mx_dcache_writeback(tex, sizeof tex); /* the DMA reads memory, not the cache */
}

/* a sprite from (x0, y0) to (x1, y1) with texels (u0, v0) to (u1, v1),
   filtered by tex1 */
static void sprite(u32 x0, u32 y0, u32 x1, u32 y1, u32 u0, u32 v0, u32 u1, u32 v1, u64 tex1) {
  u64 *c = mx_frame_direct(1 + 6);
  c = mx_e_ad_tag(c, 6, 1);
  c = mx_e_ad(c, MX_GS_REG_TEX1_1, tex1);
  /* TME 1: textured; FST 1: UV (texels), not ST (perspective) */
  c = mx_e_ad(c, MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 1, 0, 0, 0, 1, 0, 0));
  c = mx_e_ad(c, MX_GS_REG_UV, mx_gs_uv(u0 << 4, v0 << 4));
  c = mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(mx_gs_px(x0), mx_gs_py(y0), 0));
  c = mx_e_ad(c, MX_GS_REG_UV, mx_gs_uv(u1 << 4, v1 << 4));
  c = mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(mx_gs_px(x1), mx_gs_py(y1), 0));
  mx_frame_commit(c);
}

int main(void) {
  mx_gs_init(640, 224);
  mx_frame_init(ring, sizeof ring, (void *)0x70002000, 0x2000);
  make_texture();
  /* the texture goes into GS memory after the framebuffers (mx_gs.tbp, in
     blocks of 256 bytes), 64 texels wide (TBW 1: units of 64) */
  u32 tbp = mx_gs.tbp;
  mx_frame_texture(0, tbp, 1, MX_GS_PSM_CT32, TW, TW, tex);
  /* TEX0: the texture's place, format and size (log2: 64 = 2^6), TCC 1
     (use its alpha), TFX 1 (DECAL: the texel's colour as it is) */
  const u64 tex0 = mx_gs_tex0(tbp, 1, MX_GS_PSM_CT32, 6, 6, 1, 1);
  /* TEX1: MMAG and MMIN, the filter when a texel is larger (0: nearest,
     1: bilinear) or smaller than a pixel */
  const u64 nearest = mx_gs_tex1(0, 0, 0, 0, 0, 0, 0);
  const u64 bilinear = mx_gs_tex1(0, 0, 1, 1, 0, 0, 0);

  float a = 0;
  for (u32 f = 0; f < FRAMES; f++, a += 0.03f) {
    mx_frame_begin(0x18, 0x18, 0x18);
    /* the first frame waits until the texture is in GS memory */
    if (f == 0)
      mx_frame_wait_path3();
    u64 *c = mx_frame_direct(1 + 1);
    mx_frame_commit(mx_e_ad(mx_e_ad_tag(c, 1, 1), MX_GS_REG_TEX0_1, tex0));

    /* the whole texture, 1 texel a pixel (2 lines: the lines are tall) */
    sprite(16, 16, 16 + 64, 16 + 32, 0, 0, TW, TW, nearest);
    /* 12 x 12 texels of it 16 times as large: nearest (each texel a block),
       then bilinear (blended between the texels' centres) */
    sprite(100, 16, 100 + 192, 16 + 96, 26, 26, 38, 38, nearest);
    sprite(310, 16, 310 + 192, 16 + 96, 26, 26, 38, 38, bilinear);

    /* a turning square of two triangles (a strip of 4 vertices), the
       texture on it, bilinear */
    static const float cx[4] = {-1, 1, -1, 1}, cy[4] = {-1, -1, 1, 1};
    float sn = tut_sin(a), cs = tut_cos(a);
    c = mx_frame_direct(1 + 1 + 1 + 8);
    c = mx_e_ad(mx_e_ad_tag(c, 1, 0), MX_GS_REG_TEX1_1, bilinear);
    c = mx_e_giftag(c, 4, 1, (s32)mx_gs_prim(MX_GS_PRIM_TRISTRIP, 0, 1, 0, 0, 0, 1, 0, 0),
                    MX_GIF_FLG_PACKED, 2, mx_gif_regs2(MX_GIF_REG_UV, MX_GIF_REG_XYZ2));
    for (int i = 0; i < 4; i++) {
      float x = 560 + 60 * (cx[i] * cs - cy[i] * sn), y = 150 + 30 * (cx[i] * sn + cy[i] * cs);
      c = mx_e_uv(c, cx[i] < 0 ? 0 : TW << 4, cy[i] < 0 ? 0 : TW << 4);
      c = mx_e_xyz2(c, mx_gs_px(0) + (u32)(x * 16), mx_gs_py(0) + (u32)(y * 16), 0, 0);
    }
    mx_frame_commit(c);
    mx_frame_end();
  }
  tut_screenshot("host:2_texture.tga");
  MX_TEST_END(0);
  return 0;
}
