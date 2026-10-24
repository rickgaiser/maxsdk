/* Text for the demos' on-screen counters: a 5x7 font (the characters of
   hud_chars) in a 256 x 8 texture, drawn as PATH2 sprites on the frame
   pipeline (<mx/frame.h>), in lines of a 224-line screen. As in
   demos/procgen/procgen.c. */
#ifndef DEMOS_HUD_H
#define DEMOS_HUD_H

#include <mx/mx.h>

static const char hud_chars[] = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ.:%/-";
/* one byte per row, bits 4..0 left to right */
static const u8 hud_glyphs[][7] = {
    {0, 0, 0, 0, 0, 0, 0},
    {14, 17, 19, 21, 25, 17, 14}, {4, 12, 4, 4, 4, 4, 14},     {14, 17, 1, 2, 4, 8, 31},
    {31, 2, 4, 2, 1, 17, 14},     {2, 6, 10, 18, 31, 2, 2},    {31, 16, 30, 1, 1, 17, 14},
    {6, 8, 16, 30, 17, 17, 14},   {31, 1, 2, 4, 8, 8, 8},      {14, 17, 17, 14, 17, 17, 14},
    {14, 17, 17, 15, 1, 2, 12},   {14, 17, 17, 31, 17, 17, 17}, {30, 17, 17, 30, 17, 17, 30},
    {14, 17, 16, 16, 16, 17, 14}, {28, 18, 17, 17, 17, 18, 28}, {31, 16, 16, 30, 16, 16, 31},
    {31, 16, 16, 30, 16, 16, 16}, {14, 17, 16, 23, 17, 17, 15}, {17, 17, 17, 31, 17, 17, 17},
    {14, 4, 4, 4, 4, 4, 14},      {7, 2, 2, 2, 2, 18, 12},     {17, 18, 20, 24, 20, 18, 17},
    {16, 16, 16, 16, 16, 16, 31}, {17, 27, 21, 21, 17, 17, 17}, {17, 17, 25, 21, 19, 17, 17},
    {14, 17, 17, 17, 17, 17, 14}, {30, 17, 17, 30, 16, 16, 16}, {14, 17, 17, 17, 21, 18, 13},
    {30, 17, 17, 30, 20, 18, 17}, {15, 16, 16, 14, 1, 1, 30},  {31, 4, 4, 4, 4, 4, 4},
    {17, 17, 17, 17, 17, 17, 14}, {17, 17, 17, 17, 17, 10, 4}, {17, 17, 17, 21, 21, 21, 10},
    {17, 17, 10, 4, 10, 17, 17},  {17, 17, 10, 4, 4, 4, 4},    {31, 1, 2, 4, 8, 16, 31},
    {0, 0, 0, 0, 0, 12, 12},      {0, 12, 12, 0, 12, 12, 0},   {24, 25, 2, 4, 8, 19, 3},
    {1, 2, 2, 4, 8, 8, 16},       {0, 0, 0, 31, 0, 0, 0}};

static u32 hud_tex[256 * 8] MX_ALIGNED(64);
static u32 hud_tbp, hud_lines = 224;

/* Make the font and upload it to tbp by PATH3 (texture slot 0) on a frame
   pipeline that has been started; lines: the height of the screen. */
static void hud_init(u32 tbp, u32 lines) {
  for (u32 i = 0; i < 256 * 8; i++)
    hud_tex[i] = 0;
  for (u32 g = 0; g < sizeof hud_glyphs / sizeof *hud_glyphs; g++)
    for (int r = 0; r < 7; r++)
      for (int b = 0; b < 5; b++)
        if (hud_glyphs[g][r] >> (4 - b) & 1)
          hud_tex[r * 256 + g * 6 + (u32)b] = 0x80FFFFFFu;
  mx_dcache_writeback(hud_tex, sizeof hud_tex);
  hud_tbp = tbp;
  hud_lines = lines;
  mx_frame_texture(0, tbp, 4, MX_GS_PSM_CT32, 256, 8, hud_tex);
  mx_frame_wait_path3();
}

/* Text at (x, y): pixels, and lines of a 224-line screen. Characters not in
   the font are spaces. */
static void hud_text(u32 x, u32 y, const char *s) {
  u32 n = 0;
  for (const char *p = s; *p; p++)
    n++;
  if (!n)
    return;
  u64 *c = mx_frame_direct(6 + 4 * n);
  c = mx_e_ad_tag(c, 4, 1);
  c = mx_e_ad(c, MX_GS_REG_TEX0_1, mx_gs_tex0(hud_tbp, 4, MX_GS_PSM_CT32, 8, 3, 1, 0));
  c = mx_e_ad(c, MX_GS_REG_TEX1_1, mx_gs_tex1(0, 0, 0, 0, 0, 0, 0));
  c = mx_e_ad(c, MX_GS_REG_TEST_1, mx_gs_test(1, 6, 0x40, 0, 0, 0, 1, 1));
  c = mx_e_ad(c, MX_GS_REG_RGBAQ, mx_gs_rgbaq(0x80, 0x80, 0x80, 0x80, 1.0f));
  c = mx_e_giftag(c, n, 1, (s32)mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 1, 0, 0, 0, 1, 0, 0),
                  MX_GIF_FLG_PACKED, 4,
                  mx_gif_regs4(MX_GIF_REG_UV, MX_GIF_REG_XYZ2, MX_GIF_REG_UV, MX_GIF_REG_XYZ2));
  for (const char *p = s; *p; p++, x += 6) {
    u32 g = 0;
    for (u32 i = 0; hud_chars[i]; i++)
      if (hud_chars[i] == *p)
        g = i;
    /* texel centres (+ 8): the PS2's GS drops the last column and row of a
       1:1 sprite otherwise */
    c = mx_e_uv(c, g * 6 * 16 + 8, 8);
    c = mx_e_xyz2(c, mx_gs_px(x), mx_gs_py(y * hud_lines / 224), 0xFFFFFF, 0);
    c = mx_e_uv(c, (g * 6 + 5) * 16 + 8, 7 * 16 + 8);
    c = mx_e_xyz2(c, mx_gs_px(x + 5), mx_gs_py((y + 7) * hud_lines / 224), 0xFFFFFF, 0);
  }
  mx_frame_commit(c);
}

/* ---- strings ------------------------------------------------------------------ */

static char *hud_cat(char *d, const char *s) {
  while (*s)
    *d++ = *s++;
  *d = 0;
  return d;
}
static char *hud_u(char *d, u32 v) {
  char t[12];
  int n = 0;
  do
    t[n++] = (char)('0' + v % 10);
  while (v /= 10);
  while (n)
    *d++ = t[--n];
  *d = 0;
  return d;
}
/* v with one decimal of v / div: "12.3" */
static char *hud_u1(char *d, u64 v, u64 div) {
  u64 t = v * 10 / div;
  d = hud_u(d, (u32)(t / 10));
  *d++ = '.';
  *d++ = (char)('0' + t % 10);
  *d = 0;
  return d;
}

#endif
