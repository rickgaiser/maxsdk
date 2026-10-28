/* The PATH3 stream next to the VIF1 stream (mx_frame3_*, chain mode).

   Each frame (in mx_gs_init's buffers, Z24, Z test GEQUAL: more is nearer):
   - VIF1 (PATH2, context 1): the clear (a colour per frame), box A, and
     box C (Z 200) over the overlap;
   - PATH3 (context 2): box B, box D (Z 150) over the same overlap (C must
     win whichever stream is drawn first), 600 small sprites in a grid (many
     slots: some are built before VIF1 reaches the frame's start and held),
     a CALL of a GIF chain in memory (box E), and box T textured from a
     texture uploaded through the GIF queue in frame 0;
   - frame 3 has no PATH3 packets (no open or join marks).
   The last frame's buffer is read back; the frames must all have been
   shown. */

#include "t.h"

#define FRAMES 6
#define NSPR 600

static u8 slots[64 * 1024] MX_ALIGNED(64);
static u8 slots3[64 * 1024] MX_ALIGNED(64);
static u32 px[32] MX_ALIGNED(64);
static u32 tex[16 * 16] MX_ALIGNED(64);
static u64 chain_e[16] MX_ALIGNED(64);

#define RGB(r, g, b) ((u32)(r) | (u32)(g) << 8 | (u32)(b) << 16)
static u32 bg(u32 k) { return RGB(0x10 + k, 0x20, 0x30); }
static const u32 A = RGB(0xF0, 0, 0), B = RGB(0, 0xF0, 0), C = RGB(0, 0, 0xF0),
                 D = RGB(0xF0, 0xF0, 0), E = RGB(0xF0, 0, 0xF0), G = RGB(0x80, 0x80, 0x80),
                 T = RGB(0x20, 0xC0, 0x60);

static u64 *box(u64 *c, const struct mx_rt *f, int ctx, u32 x0, u32 y0, u32 x1, u32 y1, u32 z,
                u32 rgb, int eop) {
  c = mx_e_ad_tag(c, 4, eop);
  c = mx_e_ad(c, MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 0, 0, 0, 0, 0, ctx, 0));
  c = mx_e_ad(c, MX_GS_REG_RGBAQ, mx_gs_rgbaq(rgb & 0xFF, rgb >> 8 & 0xFF, rgb >> 16, 0x80, 1));
  c = mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(mx_rt_px(f, x0), mx_rt_py(f, y0), z));
  return mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(mx_rt_px(f, x1), mx_rt_py(f, y1), z));
}

static void frame(void) {
  u32 k = mx_frame_begin_n();
  const struct mx_rt *f = &mx_gs.rt[k & 1];
  mx_frame_target(f, 0);
  u64 *c = mx_frame_direct(6);
  c = mx_e_ad_tag(c, 5, 1);
  c = mx_e_ad(c, MX_GS_REG_TEST_1, mx_gs_test(0, 0, 0, 0, 0, 0, 1, 1));
  c = mx_e_ad(c, MX_GS_REG_PRMODECONT, 1);
  c = mx_e_ad(c, MX_GS_REG_COLCLAMP, 1);
  c = mx_e_ad(c, MX_GS_REG_TEXA, 0x80ull << 32);
  mx_frame_commit(mx_e_ad(c, MX_GS_REG_DTHE, 0));
  mx_frame_commit(
      mx_rt_e_clear(mx_frame_direct(5), f, 0, bg(k) & 0xFF, bg(k) >> 8 & 0xFF, bg(k) >> 16, 1));
  c = mx_frame_direct(2);
  c = mx_e_ad_tag(c, 1, 1);
  mx_frame_commit(mx_e_ad(c, MX_GS_REG_TEST_1, mx_gs_test(0, 0, 0, 0, 0, 0, 1, 2)));

  if (k != 3) {
    /* PATH3: its target and state, then the boxes (after the clear) */
    mx_frame3_target(f);
    c = mx_frame3_reserve(5);
    c = mx_e_ad_tag(c, 4, 1);
    c = mx_e_ad(c, MX_GS_REG_TEST_2, mx_gs_test(0, 0, 0, 0, 0, 0, 1, 2));
    c = mx_e_ad(c, MX_GS_REG_TEX0_2, mx_gs_tex0(mx_gs.tbp, 1, MX_GS_PSM_CT32, 4, 4, 1, 1));
    c = mx_e_ad(c, MX_GS_REG_TEX1_2, mx_gs_tex1(0, 0, 0, 0, 0, 0, 0));
    mx_frame3_commit(mx_e_ad(c, MX_GS_REG_CLAMP_2, mx_gs_clamp(1, 1, 0, 0, 0, 0)));
    mx_frame3_commit(box(mx_frame3_reserve(5), f, 1, 200, 20, 260, 60, 100, B, 1));
    mx_frame3_commit(box(mx_frame3_reserve(5), f, 1, 300, 20, 380, 60, 150, D, 1));
  }
  /* VIF1: A, and C over the overlap with D */
  mx_frame_commit(box(mx_frame_direct(5), f, 0, 20, 20, 80, 60, 100, A, 1));
  mx_frame_commit(box(mx_frame_direct(5), f, 0, 340, 20, 420, 60, 200, C, 1));
  if (k != 3) {
    for (u32 i = 0; i < NSPR; i++) {
      u32 x = 20 + (i % 60) * 10, y = 100 + (i / 60) * 10;
      mx_frame3_commit(box(mx_frame3_reserve(5), f, 1, x, y, x + 8, y + 8, 100, G, 1));
    }
    mx_frame3_call(chain_e);
    /* T: the texture, decal */
    c = mx_frame3_reserve(6);
    c = mx_e_ad_tag(c, 5, 1);
    c = mx_e_ad(c, MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 1, 0, 0, 0, 1, 1, 0));
    c = mx_e_ad(c, MX_GS_REG_UV, mx_gs_uv(0, 0));
    c = mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(mx_rt_px(f, 500), mx_rt_py(f, 20), 100));
    c = mx_e_ad(c, MX_GS_REG_UV, mx_gs_uv(16 << 4, 16 << 4));
    c = mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(mx_rt_px(f, 516), mx_rt_py(f, 36), 100));
    mx_frame3_commit(c);
  }
  mx_frame_end();
}

static u32 pixel(const struct mx_rt *f, u32 x, u32 y) {
  mx_gs_download(px, MX_GS_PAGE(f->fbp), f->fbw, MX_GS_PSM_CT32, x & ~31u, y, 32, 1);
  return px[x & 31] & 0xFFFFFF;
}

int main(void) {
  mx_gs_init(640, 224);
  /* E: a GIF chain in memory, called from the PATH3 stream */
  u64 *c = mx_e_cnt(chain_e, 5, 0, 0);
  c = box(c, &mx_gs.rt[0], 1, 440, 20, 480, 60, 100, E, 1);
  mx_e_ret(c, 0, 0, 0);
  mx_dcache_writeback(chain_e, sizeof chain_e);
  for (int i = 0; i < 256; i++)
    tex[i] = T | 0x80000000u;
  mx_dcache_writeback(tex, sizeof tex);

  CHECK_EQ(mx_frame_init_chain(slots, sizeof slots, (void *)0x70000000, 0x2000), 0);
  CHECK_EQ(mx_frame3_init(slots3, sizeof slots3, (void *)0x70002000, 0x2000), 0);
  CHECK_EQ(mx_frame_texture(0, mx_gs.tbp, 1, MX_GS_PSM_CT32, 16, 16, tex), 0);
  u32 first = mx_gs.frames;
  for (int i = 0; i < FRAMES; i++)
    frame();
  u32 last = mx_gs.frames - 1, t0 = mx_count();
  while (mx_gs.shown < last + 1 && mx_count() - t0 < 300000000u)
    ;
  mx_frame_stop();
  CHECK_EQ(mx_gs.shown, last + 1);
  mx_printf("frame3: frames %u-%u, missed %u; PATH3 %u slots, %u qw, %u held, %u cycles "
            "waiting\n",
            first, last, mx_gs.missed, mx_frame3_stats.slots, mx_frame3_stats.qw,
            mx_frame3_stats.held, mx_frame3_stats.wait_slot);

  for (u32 i = 0; i < 2; i++) {
    u32 k = last - i;
    const struct mx_rt *f = &mx_gs.rt[k & 1];
    CHECK_EQ(pixel(f, 8, 8), bg(k));
    CHECK_EQ(pixel(f, 50, 40), A);
    CHECK_EQ(pixel(f, 230, 40), B);
    CHECK_EQ(pixel(f, 320, 40), D);
    CHECK_EQ(pixel(f, 360, 40), C); /* the overlap: C (nearer) */
    CHECK_EQ(pixel(f, 400, 40), C);
    CHECK_EQ(pixel(f, 460, 40), E);
    CHECK_EQ(pixel(f, 508, 28), T);
    CHECK_EQ(pixel(f, 24, 104), G);                            /* the first sprite */
    CHECK_EQ(pixel(f, 20 + 59 * 10 + 4, 100 + 9 * 10 + 4), G); /* the last */
    CHECK_EQ(pixel(f, 29, 104), bg(k));                        /* between two */
  }
  CHECK(mx_frame3_stats.slots >= 10);
  return t_done("frame3");
}
