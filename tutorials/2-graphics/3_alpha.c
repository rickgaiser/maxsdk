/* Tutorial 2, step 3: alpha blending. With ABE in PRIM the GS mixes each
   pixel it draws (Cs, its alpha As) with the one in the framebuffer (Cd) by
   the ALPHA register: (A - B) * C / 128 + D, where A, B and D are Cs, Cd or 0
   and C is As, Ad or a fixed value. Left: (Cs - Cd) As + Cd, a transparent
   colour (alpha 0x40: a quarter of the way to it ... 0x80: all). Right:
   (Cs - 0) FIX + Cd with FIX 128, the colours added: where they overlap they
   go to white (COLCLAMP clamps at 255). */

#include "tut.h"

#define FRAMES 120
#define SEG 32 /* a circle's outer vertices */

static u8 ring[16 * 1024] MX_ALIGNED(16 * 1024);

/* a filled circle at (x, y), radius r pixels, colour and alpha, blended */
static void circle(float x, float y, float r, u32 rgb, u32 alpha) {
  u64 *c = mx_frame_direct(1 + 1 + 1 + (SEG + 2));
  c = mx_e_ad(mx_e_ad_tag(c, 1, 0), MX_GS_REG_RGBAQ,
              mx_gs_rgbaq(rgb >> 16, rgb >> 8 & 0xFF, rgb & 0xFF, alpha, 1));
  /* ABE 1: blended; one register a vertex: XYZ2 */
  c = mx_e_giftag(c, SEG + 2, 1, (s32)mx_gs_prim(MX_GS_PRIM_TRIFAN, 0, 0, 0, 1, 0, 0, 0, 0),
                  MX_GIF_FLG_PACKED, 1, MX_GIF_REG_XYZ2);
  c = mx_e_xyz2(c, mx_gs_px(0) + (u32)(x * 16), mx_gs_py(0) + (u32)(y * 16), 0, 0);
  for (int i = 0; i <= SEG; i++) {
    float t = 6.2831853f * (float)i / SEG;
    float px = x + r * tut_cos(t), py = y + r * 0.5f * tut_sin(t);
    c = mx_e_xyz2(c, mx_gs_px(0) + (u32)(px * 16), mx_gs_py(0) + (u32)(py * 16), 0, 0);
  }
  mx_frame_commit(c);
}

static void set_alpha(u64 alpha) {
  u64 *c = mx_frame_direct(1 + 1);
  mx_frame_commit(mx_e_ad(mx_e_ad_tag(c, 1, 1), MX_GS_REG_ALPHA_1, alpha));
}

int main(void) {
  mx_gs_init(640, 224);
  mx_frame_init(ring, sizeof ring, (void *)0x70002000, 0x2000);
  /* A, B, C, D (0: Cs, 1: Cd, 2: 0 / FIX), FIX */
  const u64 blend = mx_gs_alpha(0, 1, 0, 1, 0);
  const u64 add = mx_gs_alpha(0, 2, 2, 1, 128);
  static const u32 rgb[3] = {0xFF2020, 0x20FF20, 0x2020FF};

  float a = 0;
  for (u32 f = 0; f < FRAMES; f++, a += 0.03f) {
    mx_frame_begin(0x20, 0x20, 0x20);
    /* three circles turning around a point, on each side */
    set_alpha(blend);
    for (int i = 0; i < 3; i++) {
      float t = a + 2.0943951f * (float)i;
      circle(160 + 45 * tut_cos(t), 112 + 22 * tut_sin(t), 70, rgb[i], 0x60);
    }
    set_alpha(add);
    for (int i = 0; i < 3; i++) {
      float t = a + 2.0943951f * (float)i;
      circle(480 + 45 * tut_cos(t), 112 + 22 * tut_sin(t), 70, rgb[i], 0x80);
    }
    mx_frame_end();
  }
  tut_screenshot("host:3_alpha.tga");
  MX_TEST_END(0);
  return 0;
}
