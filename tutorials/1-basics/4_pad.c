/* Tutorial 1, step 4: the pad and text. The D-pad or the left stick moves a
   box; the HUD (a small font in GS memory) writes its position and the frame
   rate. START ends the program, or it ends by itself after FRAMES frames. */

#include <mx/hud.h>
#include <mx/pad.h>
#include "tut.h"

#define FRAMES 600
#define SIZE 32

static u8 ring[16 * 1024] MX_ALIGNED(16 * 1024);

static void draw_box(s32 x, s32 y, u32 r, u32 g, u32 b) {
  u64 *c = mx_frame_direct(1 + 4);
  c = mx_e_ad_tag(c, 4, 1);
  c = mx_e_ad(c, MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 0, 0, 0, 0, 0, 0, 0));
  c = mx_e_ad(c, MX_GS_REG_RGBAQ, mx_gs_rgbaq(r, g, b, 0x80, 1));
  c = mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(mx_gs_px((u32)x), mx_gs_py((u32)y), 0));
  c = mx_e_ad(c, MX_GS_REG_XYZ2,
              mx_gs_xyz(mx_gs_px((u32)(x + SIZE)), mx_gs_py((u32)(y + SIZE / 2)), 0));
  mx_frame_commit(c);
}

int main(void) {
  mx_gs_init(640, 224);
  mx_frame_init(ring, sizeof ring, (void *)0x70002000, 0x2000);
  /* the font: a texture in the GS memory after the framebuffers
     (mx_gs.tbp), for a screen of 224 lines */
  mx_hud_init(mx_gs.tbp, 224);
  /* the pad driver (the BIOS's): port 0 and 1 */
  if (mx_pad_init() < 0)
    mx_printf("no pad driver\n");

  s32 x = 320 - SIZE / 2, y = 112 - SIZE / 4;
  u32 v_last = mx_gs.vsync, frames_last = 0, fps = 0;
  char line[64];
  for (u32 f = 0; f < FRAMES; f++) {
    u32 held = mx_pad_buttons(0), pressed = mx_pad_pressed(0);
    if (pressed & MX_PAD_START)
      break;
    /* the D-pad: 4 pixels a frame (2 lines down: the lines are twice as
       tall as the pixels are wide) */
    x += held & MX_PAD_RIGHT ? 4 : held & MX_PAD_LEFT ? -4 : 0;
    y += held & MX_PAD_DOWN ? 2 : held & MX_PAD_UP ? -2 : 0;
    /* the left stick, if the pad is analog: 128 is the middle. A stick at
       rest is rarely exactly there, so small values count as 0 (a dead
       zone). */
    u8 st[4];
    if (mx_pad_sticks(0, st)) {
      s32 sx = (s32)st[2] - 128, sy = (s32)st[3] - 128;
      sx = sx > -40 && sx < 40 ? 0 : sx, sy = sy > -40 && sy < 40 ? 0 : sy;
      x += sx / 24, y += sy / 48;
    }
    x = x < 0 ? 0 : x > 640 - SIZE ? 640 - SIZE : x;
    y = y < 0 ? 0 : y > 224 - SIZE / 2 ? 224 - SIZE / 2 : y;

    /* frames a second, counted over each second of vblanks */
    if (mx_gs.vsync - v_last >= mx_gs.hz) {
      fps = f - frames_last, frames_last = f, v_last = mx_gs.vsync;
    }

    mx_frame_begin(0x10, 0x18, 0x10);
    draw_box(x, y, 0xFF, 0x80, 0x20);
    /* text: x in pixels, y in lines of a 224-line screen, the colour
       0xRRGGBB (0x80 is full), the size */
    mx_hud_text(16, 8, "TUTORIAL 1: THE PAD", 0x808080, 2);
    char *p = mx_hud_cat(line, "X ");
    p = mx_hud_cat(mx_hud_u(p, (u32)x), "  Y ");
    p = mx_hud_cat(mx_hud_u(p, (u32)y), "  ");
    mx_hud_cat(mx_hud_u(p, fps), " FPS");
    mx_hud_text(16, 30, line, 0x60FF60, 1);
    mx_hud_text(16, 200, mx_pad_connected(0) ? "D-PAD OR STICK: MOVE   START: END" : "NO PAD",
                0x606060, 1);
    mx_frame_end();
  }
  tut_screenshot("host:4_pad.tga");
  MX_TEST_END(0);
  return 0;
}
