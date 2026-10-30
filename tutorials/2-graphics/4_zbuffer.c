/* Tutorial 2, step 4: the Z buffer. Each vertex has a depth, Z (more is
   nearer here), and the GS keeps the Z of every pixel in the Z buffer. With
   the Z test on (TEST: ZTE, ZTST GEQUAL) a pixel is drawn only where it is
   at least as near as what is there, so the order of drawing no longer
   matters. Left: two triangles that cut through each other, Z test off
   (ZTST ALWAYS): the one drawn last covers the other. Right: the same with
   the test on: each shows where it is in front. */

#include "tut.h"

#define FRAMES 60

static u8 ring[16 * 1024] MX_ALIGNED(16 * 1024);

struct vtx {
  float x, y;
  u32 z;
};

static void triangle(const struct vtx *v, float dx, u32 rgb) {
  u64 *c = mx_frame_direct(1 + 1 + 1 + 3);
  c = mx_e_ad(mx_e_ad_tag(c, 1, 0), MX_GS_REG_RGBAQ,
              mx_gs_rgbaq(rgb >> 16, rgb >> 8 & 0xFF, rgb & 0xFF, 0x80, 1));
  c = mx_e_giftag(c, 3, 1, (s32)mx_gs_prim(MX_GS_PRIM_TRIANGLE, 0, 0, 0, 0, 0, 0, 0, 0),
                  MX_GIF_FLG_PACKED, 1, MX_GIF_REG_XYZ2);
  for (int i = 0; i < 3; i++)
    c = mx_e_xyz2(c, mx_gs_px(0) + (u32)((v[i].x + dx) * 16), mx_gs_py(0) + (u32)(v[i].y * 16),
                  v[i].z, 0);
  mx_frame_commit(c);
}

/* TEST: ATE, ATST, AREF, AFAIL (the alpha test), DATE, DATM, ZTE, ZTST
   (1: always, 2: greater or equal) */
static void z_test(u32 ztst) {
  u64 *c = mx_frame_direct(1 + 1);
  mx_frame_commit(
      mx_e_ad(mx_e_ad_tag(c, 1, 1), MX_GS_REG_TEST_1, mx_gs_test(0, 0, 0, 0, 0, 0, 1, ztst)));
}

int main(void) {
  mx_gs_init(640, 224);
  mx_frame_init(ring, sizeof ring, (void *)0x70002000, 0x2000);

  /* A leans back from left to right, B the other way: they cross in the
     middle. Z is 24 bits here (0 .. 0xFFFFFF); the clear sets it to 0. */
  static const struct vtx a[3] = {{40, 40, 0x100000}, {280, 110, 0x900000}, {40, 180, 0x100000}};
  static const struct vtx b[3] = {{280, 40, 0x100000}, {40, 110, 0x900000}, {280, 180, 0x100000}};

  for (u32 f = 0; f < FRAMES; f++) {
    /* mx_frame_begin's clear writes Z 0 too (the test is off for it) */
    mx_frame_begin(0x10, 0x10, 0x20);
    z_test(1);
    triangle(a, 0, 0xE04040);
    triangle(b, 0, 0x4060E0);
    z_test(2);
    triangle(a, 320, 0xE04040);
    triangle(b, 320, 0x4060E0);
    mx_frame_end();
  }
  tut_screenshot("host:4_zbuffer.tga");
  MX_TEST_END(0);
  return 0;
}
