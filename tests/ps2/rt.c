/* Render targets in both GS contexts and three display states
   (<mx/rt.h>, <mx/display.h>), with mx_gs_init_video instead of mx_gs_init.

   Each frame, drawn in the frame's own buffer (display k % 3) in context 1:
   (a) context 2 draws into a 128 x 128 CT32 target (a clear and a sprite),
       switched by PRIM.CTXT only; after a TEXFLUSH, context 1 copies the
       target into the frame (copy 1);
   (b) context 1 is moved to the target (mx_frame_target), draws a corner,
       moves back, and copies the target again (copy 2);
   (c) the same as (a) with a CT16S target (copy 3).
   The frame's background colour is its number, so after the run the three
   buffers hold the last three frames. Run in ring and in chain mode. */

#include "t.h"

#define FRAMES 6
#define NBUF 3
#define SZ 128

static u8 ring[64 * 1024] MX_ALIGNED(64 * 1024);
static u8 slots[64 * 1024] MX_ALIGNED(64);
static u32 px[32] MX_ALIGNED(64);

static struct mx_rt fb[NBUF], tgt, tgt16;
static struct mx_display disp[NBUF];

#define RGB(r, g, b) ((u32)(r) | (u32)(g) << 8 | (u32)(b) << 16)
static u32 bg(u32 k) { return RGB(0x10 + k, 0x20, 0x30); }
static const u32 A = RGB(0x40, 0x80, 0xC0), B = RGB(0xF0, 0x10, 0x20), C = RGB(0x11, 0xEE, 0x22);
/* exact in 5 bits a channel */
static const u32 D = RGB(0xF8, 0x00, 0x80), E = RGB(0x08, 0xF8, 0x40);

/* a flat sprite of context ctx over window pixels (x0, y0)-(x1, y1) of t */
static void sprite(const struct mx_rt *t, int ctx, u32 x0, u32 y0, u32 x1, u32 y1, u32 rgb) {
  u64 *c = mx_frame_direct(5);
  c = mx_e_ad_tag(c, 4, 1);
  c = mx_e_ad(c, MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 0, 0, 0, 0, 0, ctx, 0));
  c = mx_e_ad(c, MX_GS_REG_RGBAQ, mx_gs_rgbaq(rgb & 0xFF, rgb >> 8 & 0xFF, rgb >> 16, 0x80, 1));
  c = mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(mx_rt_px(t, x0), mx_rt_py(t, y0), 0));
  mx_frame_commit(mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(mx_rt_px(t, x1), mx_rt_py(t, y1), 0)));
}

static void clear(const struct mx_rt *t, int ctx, u32 rgb) {
  u64 *c = mx_frame_direct(5);
  mx_frame_commit(mx_rt_e_clear(c, t, ctx, rgb & 0xFF, rgb >> 8 & 0xFF, rgb >> 16, 1));
}

/* src (a target) 1:1 into the frame at (x, y), context 1, decal */
static void copy(const struct mx_rt *src, const struct mx_rt *dst, u32 x, u32 y) {
  u64 *c = mx_frame_direct(8);
  c = mx_e_ad_tag(c, 7, 1);
  c = mx_e_ad(c, MX_GS_REG_TEX0_1, mx_rt_tex0(src, 0, 1));
  c = mx_e_ad(c, MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 1, 0, 0, 0, 1, 0, 0));
  c = mx_e_ad(c, MX_GS_REG_RGBAQ, mx_gs_rgbaq(0x80, 0x80, 0x80, 0x80, 1));
  c = mx_e_ad(c, MX_GS_REG_UV, mx_gs_uv(0, 0));
  c = mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(mx_rt_px(dst, x), mx_rt_py(dst, y), 0));
  c = mx_e_ad(c, MX_GS_REG_UV, mx_gs_uv(SZ << 4, SZ << 4));
  mx_frame_commit(
      mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(mx_rt_px(dst, x + SZ), mx_rt_py(dst, y + SZ), 0)));
}

static void frame(void) {
  u32 k = mx_frame_begin_n();
  const struct mx_rt *f = &fb[k % NBUF];
  mx_frame_target(f, 0);
  mx_frame_target(&tgt, 1);
  /* the state that gs.c's draw environment sets, for both contexts (the
     mode set reset the GS) */
  u64 *c = mx_frame_direct(8);
  c = mx_e_ad_tag(c, 7, 1);
  c = mx_e_ad(c, MX_GS_REG_TEST_1, mx_gs_test(0, 0, 0, 0, 0, 0, 1, 1));
  c = mx_e_ad(c, MX_GS_REG_TEST_2, mx_gs_test(0, 0, 0, 0, 0, 0, 1, 1));
  c = mx_e_ad(c, MX_GS_REG_PRMODECONT, 1);
  c = mx_e_ad(c, MX_GS_REG_COLCLAMP, 1);
  c = mx_e_ad(c, MX_GS_REG_DTHE, 0);
  c = mx_e_ad(c, MX_GS_REG_TEX1_1, mx_gs_tex1(0, 0, 0, 0, 0, 0, 0));
  mx_frame_commit(mx_e_ad(c, MX_GS_REG_TEXA, 0x80ull << 32));
  clear(f, 0, bg(k));

  /* (a) context 2 into the target, context 1 copies it */
  clear(&tgt, 1, A);
  sprite(&tgt, 1, 32, 32, 96, 96, B);
  mx_frame_texflush();
  copy(&tgt, f, 40, 40);

  /* (b) context 1 into the target and back */
  mx_frame_target(&tgt, 0);
  sprite(&tgt, 0, 0, 0, 32, 32, C);
  mx_frame_target(f, 0);
  mx_frame_texflush();
  copy(&tgt, f, 200, 40);

  /* (c) a 16-bit target in context 2 */
  mx_frame_target(&tgt16, 1);
  clear(&tgt16, 1, D);
  sprite(&tgt16, 1, 32, 32, 96, 96, E);
  mx_frame_texflush();
  copy(&tgt16, f, 360, 40);
  mx_frame_end();
}

static u32 pixel(const struct mx_rt *t, u32 x, u32 y) {
  mx_gs_download(px, MX_GS_PAGE(t->fbp), t->fbw, MX_GS_PSM_CT32, x & ~31u, y, 32, 1);
  return px[x & 31] & 0xFFFFFF;
}

static void run(int chain) {
  if (chain)
    CHECK_EQ(mx_frame_init_chain(slots, sizeof slots, (void *)0x70002000, 0x2000), 0);
  else
    mx_frame_init(ring, sizeof ring, (void *)0x70002000, 0x2000);
  u32 first = mx_gs.frames;
  for (int i = 0; i < FRAMES; i++)
    frame();
  u32 last = mx_gs.frames - 1, t0 = mx_count();
  while (mx_gs.shown < last + 1 && mx_count() - t0 < 300000000u)
    ;
  mx_frame_stop();
  CHECK_EQ(mx_gs.shown, last + 1);
  CHECK_EQ(mx_gs.shown_fbp, fb[last % NBUF].fbp);
  mx_printf("rt: %s mode, frames %u-%u, missed %u\n", chain ? "chain" : "ring", first, last,
            mx_gs.missed);

  /* the three buffers: the last three frames */
  for (u32 i = 0; i < NBUF; i++) {
    u32 k = last - i;
    CHECK_EQ(pixel(&fb[k % NBUF], 8, 8), bg(k));
  }
  const struct mx_rt *f = &fb[last % NBUF];
  CHECK_EQ(pixel(f, 40 + 10, 40 + 10), A); /* copy 1: before the corner */
  CHECK_EQ(pixel(f, 40 + 64, 40 + 64), B);
  CHECK_EQ(pixel(f, 40 + 127, 40 + 127), A);
  CHECK_EQ(pixel(f, 40 + 128, 40 + 64), bg(last));
  CHECK_EQ(pixel(f, 200 + 10, 40 + 10), C); /* copy 2: with it */
  CHECK_EQ(pixel(f, 200 + 64, 40 + 64), B);
  CHECK_EQ(pixel(f, 200 + 40, 40 + 10), A);
  CHECK_EQ(pixel(f, 360 + 10, 40 + 10), D); /* copy 3: 16 bits */
  CHECK_EQ(pixel(f, 360 + 64, 40 + 64), E);
  /* the target itself */
  CHECK_EQ(pixel(&tgt, 64, 64), B);
}

int main(void) {
  struct mx_video_mode m = mx_video_ntsc;
  m.ffmd = 1;
  mx_gs_init_video(&m);
  u32 pages = mx_rt_pages(640, 224, MX_GS_PSM_CT32);
  u32 z = mx_vram_alloc(pages);
  for (int i = 0; i < NBUF; i++) {
    mx_rt_init(&fb[i], mx_vram_alloc(pages), 640, 224, MX_GS_PSM_CT32, z, MX_GS_PSM_Z24);
    mx_display_init(&disp[i], &fb[i]);
  }
  mx_rt_init(&tgt, mx_vram_alloc(mx_rt_pages(SZ, SZ, MX_GS_PSM_CT32)), SZ, SZ, MX_GS_PSM_CT32, 0,
             MX_RT_NOZ);
  mx_rt_init(&tgt16, mx_vram_alloc(mx_rt_pages(SZ, SZ, MX_GS_PSM_CT16S)), SZ, SZ, MX_GS_PSM_CT16S,
             0, MX_RT_NOZ);
  CHECK_EQ(mx_vram_alloc(512), ~0u);
  mx_printf("rt: %u pages used, textures from block %u\n", mx_gs.vram, mx_gs.tbp);
  mx_gs_displays(disp, NBUF);
  mx_display_apply(&disp[0]);

  run(0);
  run(1);
  return t_done("rt");
}
