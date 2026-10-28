/* Textures with MIPMAP levels (<mx/tex.h>): block counts, level layout,
   register values, K. */

#include <mx/bits.h>
#include <mx/tex.h>
#include "test.h"

static void blocks(void) {
  /* a page or more: whole pages */
  CHECK_EQ(mx_tex_blocks(64, 32, MX_GS_PSM_CT32), 32);
  CHECK_EQ(mx_tex_blocks(256, 256, MX_GS_PSM_CT32), 32 * 32);
  CHECK_EQ(mx_tex_blocks(128, 16, MX_GS_PSM_CT32), 64);
  CHECK_EQ(mx_tex_blocks(32, 64, MX_GS_PSM_CT32), 64);
  CHECK_EQ(mx_tex_blocks(64, 64, MX_GS_PSM_CT16), 32);
  CHECK_EQ(mx_tex_blocks(128, 64, MX_GS_PSM_T8), 32);
  CHECK_EQ(mx_tex_blocks(128, 128, MX_GS_PSM_T4), 32);
  /* under a page: up to the last block of the page's order (blocks: CT32
     8 x 8, CT16 16 x 8, T8 16 x 16, T4 32 x 16) */
  CHECK_EQ(mx_tex_blocks(8, 8, MX_GS_PSM_CT32), 1);
  CHECK_EQ(mx_tex_blocks(16, 16, MX_GS_PSM_CT32), 4);
  CHECK_EQ(mx_tex_blocks(32, 16, MX_GS_PSM_CT32), 8);
  CHECK_EQ(mx_tex_blocks(32, 32, MX_GS_PSM_CT32), 16);
  CHECK_EQ(mx_tex_blocks(64, 8, MX_GS_PSM_CT32), 22); /* row 0: 0 1 4 5 16 17 20 21 */
  CHECK_EQ(mx_tex_blocks(32, 32, MX_GS_PSM_CT16), 8);
  CHECK_EQ(mx_tex_blocks(16, 16, MX_GS_PSM_CT16), 2);
  CHECK_EQ(mx_tex_blocks(32, 32, MX_GS_PSM_CT16S), 12); /* 0 2 / 1 3 / 8 10 / 9 11 */
  CHECK_EQ(mx_tex_blocks(64, 32, MX_GS_PSM_T8), 8);
  CHECK_EQ(mx_tex_blocks(8, 8, MX_GS_PSM_T8), 1);
  CHECK_EQ(mx_tex_blocks(64, 64, MX_GS_PSM_T4), 8);
  CHECK_EQ(mx_tex_blocks(8, 8, MX_GS_PSM_Z24), 32); /* Z: whole pages */

  CHECK_EQ(mx_tex_tbw(8, MX_GS_PSM_CT32), 1);
  CHECK_EQ(mx_tex_tbw(256, MX_GS_PSM_CT32), 4);
  CHECK_EQ(mx_tex_tbw(8, MX_GS_PSM_T8), 2);
  CHECK_EQ(mx_tex_tbw(256, MX_GS_PSM_T4), 4);

  CHECK_EQ(mx_tex_max_levels(9, 9, 8), 7);
  CHECK_EQ(mx_tex_max_levels(6, 6, 8), 4); /* 64 32 16 8 */
  CHECK_EQ(mx_tex_max_levels(8, 5, 8), 3); /* 256 x 32 .. 64 x 8 */
  CHECK_EQ(mx_tex_max_levels(3, 3, 8), 1);
  CHECK_EQ(mx_tex_max_levels(2, 2, 8), 1);
  CHECK_EQ(mx_tex_max_levels(4, 4, 1), 5);
}

static void layout(void) {
  struct mx_tex t;
  /* the GS manual's example (3.4.11): 512 x 512 CT16 at 7200 */
  static const u32 tbp[7] = {7200, 9248, 9760, 9888, 9920, 9928, 9930};
  static const u32 tbw[7] = {8, 4, 2, 1, 1, 1, 1};
  CHECK_EQ(mx_tex_init(&t, 7200, MX_GS_PSM_CT16, 9, 9, 7, MX_TEX_PACKED), 9931 - 7200);
  for (int n = 0; n < 7; n++) {
    CHECK_EQ(t.tbp[n], tbp[n]);
    CHECK_EQ(t.tbw[n], tbw[n]);
  }
  CHECK_EQ(mx_tex_miptbp1(&t), MX_SET(GS_MIPTBP1_TBP1, 9248) | MX_SET(GS_MIPTBP1_TBW1, 4) |
                                   MX_SET(GS_MIPTBP1_TBP2, 9760) | MX_SET(GS_MIPTBP1_TBW2, 2) |
                                   MX_SET(GS_MIPTBP1_TBP3, 9888) | MX_SET(GS_MIPTBP1_TBW3, 1));
  CHECK_EQ(mx_tex_miptbp2(&t), MX_SET(GS_MIPTBP2_TBP4, 9920) | MX_SET(GS_MIPTBP2_TBW4, 1) |
                                   MX_SET(GS_MIPTBP2_TBP5, 9928) | MX_SET(GS_MIPTBP2_TBW5, 1) |
                                   MX_SET(GS_MIPTBP2_TBP6, 9930) | MX_SET(GS_MIPTBP2_TBW6, 1));

  /* CT32 64 x 64, 4 levels: 2 pages, then 32 x 32 (16 blocks), 16 x 16 (4), 8 x 8 (1) */
  CHECK_EQ(mx_tex_init(&t, 100 * 32, MX_GS_PSM_CT32, 6, 6, 4, MX_TEX_PACKED), 64 + 16 + 4 + 1);
  CHECK_EQ(t.tbp[1], 100 * 32 + 64);
  CHECK_EQ(t.tbp[2], 100 * 32 + 80);
  CHECK_EQ(t.tbp[3], 100 * 32 + 84);
  CHECK_EQ(mx_tex_miptbp2(&t), 0);
  /* a rectangle: 128 x 16 two pages wide, 64 x 8 22 blocks */
  mx_tex_init(&t, 0, MX_GS_PSM_CT32, 7, 4, 2, MX_TEX_PACKED);
  CHECK_EQ(t.tbp[1], 64);
  CHECK_EQ(t.blocks, 64 + 22);
  /* T8 128 x 128: 2 pages, then 64 x 64 (16 blocks), 32 x 32 (4: 0 1 / 2 3),
     16 x 16 and 8 x 8 (1) */
  mx_tex_init(&t, 0, MX_GS_PSM_T8, 7, 7, 5, MX_TEX_PACKED);
  CHECK_EQ(t.tbp[1], 64);
  CHECK_EQ(t.tbp[2], 80);
  CHECK_EQ(t.tbp[3], 84);
  CHECK_EQ(t.tbp[4], 85);
  CHECK_EQ(t.tbw[4], 2);
  /* every level from a page */
  CHECK_EQ(mx_tex_init(&t, 64, MX_GS_PSM_CT32, 6, 6, 4, MX_TEX_PAGES), 5 * 32);
  CHECK_EQ(t.tbp[1], 64 + 64);
  CHECK_EQ(t.tbp[2], 64 + 96);
  CHECK_EQ(t.tbp[3], 64 + 128);
  struct mx_rt rt;
  mx_tex_rt(&t, 2, &rt);
  CHECK_EQ(rt.fbp, (64 + 96) / 32);
  CHECK_EQ(rt.fbw, 1);
  CHECK_EQ(rt.w, 16);
}

static void regs(void) {
  struct mx_tex t;
  mx_tex_init(&t, 0, MX_GS_PSM_T8, 7, 7, 5, MX_TEX_PACKED);
  t.cbp = 500;
  CHECK_EQ(mx_tex_tex0(&t, 1, 0), mx_gs_tex0(0, 2, MX_GS_PSM_T8, 7, 7, 1, 0) |
                                      MX_SET(GS_TEX0_CBP, 500) | MX_SET(GS_TEX0_CLD, 1));
  u64 tex1 = mx_tex_tex1(&t, 1, 4, 1, -40);
  CHECK_EQ(MX_GET(GS_TEX1_LCM, tex1), 0);
  CHECK_EQ(MX_GET(GS_TEX1_MXL, tex1), 4);
  CHECK_EQ(MX_GET(GS_TEX1_MMAG, tex1), 1);
  CHECK_EQ(MX_GET(GS_TEX1_MMIN, tex1), 4);
  CHECK_EQ(MX_GET(GS_TEX1_L, tex1), 1);
  CHECK_EQ(MX_GET(GS_TEX1_K, tex1), 0x1000 - 40);
  CHECK_EQ(tex1 >> 44, 0);

  static u64 a[8] __attribute__((aligned(16)));
  CHECK_EQ(mx_tex_e_regs(a, &t, 1, 11, 22) - a, 8);
  CHECK_EQ(a[0], 22);
  CHECK_EQ(a[1], MX_GS_REG_TEX1_2);
  CHECK_EQ(a[3], MX_GS_REG_TEX0_2);
  CHECK_EQ(a[4], mx_tex_miptbp1(&t));
  CHECK_EQ(a[5], MX_GS_REG_MIPTBP1_2);
  CHECK_EQ(a[7], MX_GS_REG_MIPTBP2_2);

  /* K: 4 texels a unit, 64 pixels a unit at w = 1: level 0 at w = 16 */
  CHECK_EQ(mx_tex_lod_k(4.0f, 64.0f, 0), -64);
  CHECK_EQ(mx_tex_lod_k(4.0f, 64.0f, 1), -128);
  CHECK_EQ(mx_tex_lod_k(3.0f, 1.0f, 0), 25);               /* log2 3 = 1.585 */
  CHECK_EQ(mx_tex_lod_k(1.0f, 1099511627776.0f, 0), -640); /* 2^40 */
  CHECK_EQ(mx_tex_lod_k(1e30f, 1e-30f, 0), 2047);
  CHECK_EQ(mx_tex_lod_k(0.0f, 1.0f, 0), 0);

  /* the GS's own levels: a tag and 14 registers a level */
  static u64 m[2 * MX_TEX_MIPGEN_QW(4)] __attribute__((aligned(16)));
  mx_tex_init(&t, 64, MX_GS_PSM_CT32, 6, 6, 4, MX_TEX_PAGES);
  CHECK_EQ(mx_tex_e_mipgen(m, &t, 0, 1) - m, 2 * MX_TEX_MIPGEN_QW(4));
  CHECK_EQ(m[0], mx_giftag(14, 0, -1, MX_GIF_FLG_PACKED, 1));
  CHECK_EQ(MX_GET(GIFTAG_EOP, m[2 * 30]), 1);
  CHECK_EQ(m[2], mx_gs_frame(4, 1, MX_GS_PSM_CT32, 0)); /* level 1 at block 128 */
  CHECK_EQ(m[12], mx_gs_tex0(64, 1, MX_GS_PSM_CT32, 6, 6, 1, 1));
}

void test_tex(void) {
  blocks();
  layout();
  regs();
}
