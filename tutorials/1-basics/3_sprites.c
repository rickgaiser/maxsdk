/* Tutorial 1, step 3: drawing. The GS draws what it is sent as GIF packets:
   a GIF tag, then register writes. Here each write is an A+D pair (the
   value, then the register's number): PRIM says what to draw, RGBAQ sets
   the colour, and each XYZ2 is a vertex. A sprite (a rectangle) takes two
   vertices, its corners.

   Coordinates are 12.4 fixed point (16ths of a pixel) in the GS's 4096 x
   4096 space; mx_gs_px and mx_gs_py turn a pixel of the screen into them. */

#include "tut.h"

#define FRAMES 240
#define N 12 /* the boxes */
#define SIZE 24

static u8 ring[16 * 1024] MX_ALIGNED(16 * 1024);

static struct {
  s32 x, y, dx, dy;
  u32 rgb;
} box[N];

/* one box, a sprite: a GIF tag for 4 A+D writes, then the writes */
static void draw_box(u32 x, u32 y, u32 rgb) {
  u64 *c = mx_frame_direct(1 + 4); /* room for the tag and 4 quadwords */
  c = mx_e_ad_tag(c, 4, 1);
  c = mx_e_ad(c, MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 0, 0, 0, 0, 0, 0, 0));
  c = mx_e_ad(c, MX_GS_REG_RGBAQ, mx_gs_rgbaq(rgb >> 16, rgb >> 8 & 0xFF, rgb & 0xFF, 0x80, 1));
  c = mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(mx_gs_px(x), mx_gs_py(y), 0));
  c = mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(mx_gs_px(x + SIZE), mx_gs_py(y + SIZE / 2), 0));
  mx_frame_commit(c);
}

int main(void) {
  mx_gs_init(640, 224);
  mx_frame_init(ring, sizeof ring, (void *)0x70002000, 0x2000);

  /* the boxes start in a row, each with its own speed and colour (the
     screen has 224 lines, so a box SIZE pixels wide is SIZE / 2 lines tall
     to look square) */
  static const u32 colours[4] = {0xFF4040, 0x40FF40, 0x4080FF, 0xFFD040};
  for (int i = 0; i < N; i++) {
    box[i].x = 40 + 48 * i, box[i].y = 100;
    box[i].dx = 1 + i % 3, box[i].dy = i & 1 ? 2 : -1;
    box[i].rgb = colours[i % 4];
  }

  for (u32 f = 0; f < FRAMES; f++) {
    mx_frame_begin(0x10, 0x10, 0x30);
    for (int i = 0; i < N; i++) {
      draw_box((u32)box[i].x, (u32)box[i].y, box[i].rgb);
      /* move, bouncing off the edges */
      box[i].x += box[i].dx, box[i].y += box[i].dy;
      if (box[i].x < 0 || box[i].x > 640 - SIZE)
        box[i].dx = -box[i].dx, box[i].x += 2 * box[i].dx;
      if (box[i].y < 0 || box[i].y > 224 - SIZE / 2)
        box[i].dy = -box[i].dy, box[i].y += 2 * box[i].dy;
    }
    mx_frame_end();
  }
  tut_screenshot("host:3_sprites.tga");
  MX_TEST_END(0);
  return 0;
}
