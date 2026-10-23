/* Phase 3: GS setup, PATH3 drawing, SIGNAL/LABEL/FINISH interrupts, texture
   upload, PATH1 (VU1 XGKICK), the double-buffered frame loop, and VRAM
   readback to check the pixels. */

#include "t.h"

#define RED 0x800000FFu
#define BLUE 0x80FF0000u
#define GREEN 0x8000FF00u

static u32 pk[2048] MX_ALIGNED(64);
static u32 px[256] MX_ALIGNED(64);
static u32 tex[16 * 16] MX_ALIGNED(64);
extern const u8 kick_vu[], kick_vu_end[];

static void send(struct mx_pkt *p) {
  CHECK(mx_pkt_finish(p) > 0);
  mx_pkt_writeback(p);
  mx_gs_send(mx_pkt_start(p));
}

static u32 pixel(u32 x, u32 y) {
  /* 32 x 1 pixels = 128 bytes, aligned to 32 */
  u32 x0 = x & ~31u;
  if (mx_gs_download(px, MX_GS_PAGE(mx_gs.fbp[0]), mx_gs.fbw, MX_GS_PSM_CT32, x0, y, 32, 1) != 0) {
    mx_printf("download failed\n");
    t_fail++;
  }
  return px[x - x0];
}

static void path3_triangle(void) {
  struct mx_pkt p;
  mx_pkt_begin(&p, pk, sizeof pk, 0);
  mx_pkt_cnt(&p, 0, 0);
  mx_gs_draw_env(&p, 0);
  mx_gs_clear(&p, 0, 0, 255);
  mx_pkt_ad_begin(&p, 5, 1);
  mx_pkt_ad(&p, MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_TRIANGLE, 0, 0, 0, 0, 0, 0, 0, 0));
  mx_pkt_ad(&p, MX_GS_REG_RGBAQ, mx_gs_rgbaq(255, 0, 0, 0x80, 1.0f));
  mx_pkt_ad(&p, MX_GS_REG_XYZ2, mx_gs_xyz(mx_gs_px(90), mx_gs_py(40), 0));
  mx_pkt_ad(&p, MX_GS_REG_XYZ2, mx_gs_xyz(mx_gs_px(130), mx_gs_py(40), 0));
  mx_pkt_ad(&p, MX_GS_REG_XYZ2, mx_gs_xyz(mx_gs_px(110), mx_gs_py(70), 0));
  mx_gs_finish_ad(&p);
  mx_pkt_end(&p, 0, 0);
  u32 f0 = mx_gs.finish;
  send(&p);
  CHECK(mx_gs_wait_finish(f0, 1) == 0);
  CHECK_EQ(pixel(110, 45), RED);
  CHECK_EQ(pixel(10, 10), BLUE);
  CHECK_EQ(pixel(110, 100), BLUE);
}

static void signal_label(void) {
  struct mx_pkt p;
  mx_pkt_begin(&p, pk, sizeof pk, 0);
  mx_pkt_cnt(&p, 0, 0);
  mx_gs_signal_ad(&p, 0x1234);
  mx_gs_label_ad(&p, 0x5678);
  mx_gs_finish_ad(&p);
  mx_pkt_end(&p, 0, 0);
  u32 s0 = mx_gs.signal, f0 = mx_gs.finish;
  send(&p);
  CHECK(mx_gs_wait_finish(f0, 1) == 0);
  CHECK_EQ(mx_gs.signal - s0, 1);
  CHECK_EQ(mx_gs.last_sigid, 0x1234);
  CHECK_EQ(MX_R64(MX_GS_SIGLBLID) >> 32, 0x5678);
}

static void texture(void) {
  for (u32 y = 0; y < 16; y++)
    for (u32 x = 0; x < 16; x++)
      tex[y * 16 + x] = 0x80000000u | (y * 16 + x) << 8 | (x * 16);
  mx_dcache_writeback(tex, sizeof tex);
  struct mx_pkt p;
  mx_pkt_begin(&p, pk, sizeof pk, 0);
  mx_pkt_cnt(&p, 0, 0);
  mx_gs_upload(&p, mx_gs.tbp, 1, MX_GS_PSM_CT32, 0, 0, 16, 16, tex);
  mx_pkt_ad_begin(&p, 6, 1);
  mx_pkt_ad(&p, MX_GS_REG_TEX0_1, mx_gs_tex0(mx_gs.tbp, 1, MX_GS_PSM_CT32, 4, 4, 1, 1));
  mx_pkt_ad(&p, MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 1, 0, 0, 0, 1, 0, 0));
  mx_pkt_ad(&p, MX_GS_REG_UV, mx_gs_uv(0, 0));
  mx_pkt_ad(&p, MX_GS_REG_XYZ2, mx_gs_xyz(mx_gs_px(200), mx_gs_py(100), 0));
  mx_pkt_ad(&p, MX_GS_REG_UV, mx_gs_uv(16 << 4, 16 << 4));
  mx_pkt_ad(&p, MX_GS_REG_XYZ2, mx_gs_xyz(mx_gs_px(216), mx_gs_py(116), 0));
  mx_gs_finish_ad(&p);
  mx_pkt_end(&p, 0, 0);
  u32 f0 = mx_gs.finish;
  send(&p);
  CHECK(mx_gs_wait_finish(f0, 1) == 0);
  /* row 5 of the sprite: pixels 192..223, the sprite is at 200..215 */
  mx_gs_download(px, MX_GS_PAGE(mx_gs.fbp[0]), mx_gs.fbw, MX_GS_PSM_CT32, 192, 105, 32, 1);
  int bad = 0;
  for (u32 x = 0; x < 16; x++)
    bad += px[8 + x] != tex[5 * 16 + x];
  if (bad)
    mx_printf("texture row: %08x %08x .. expected %08x %08x\n", px[8], px[9], tex[80], tex[81]);
  CHECK_EQ(bad, 0);
}

/* PATH1: a GIF packet unpacked into VU1 memory, sent by XGKICK. */
static void path1_vu1(void) {
  static u32 gif[4 * 4] MX_ALIGNED(16);
  u64 *g = (u64 *)gif;
  g[0] = mx_giftag(1, 1, (s32)mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 0, 0, 0, 0, 0, 0, 0),
                   MX_GIF_FLG_PACKED, 3);
  g[1] = mx_gif_regs3(MX_GIF_REG_RGBAQ, MX_GIF_REG_XYZ2, MX_GIF_REG_XYZ2);
  gif[4] = 0, gif[5] = 255, gif[6] = 0, gif[7] = 0x80;             /* RGBA */
  gif[8] = mx_gs_px(300), gif[9] = mx_gs_py(150), gif[10] = 0, gif[11] = 0;
  gif[12] = mx_gs_px(320), gif[13] = mx_gs_py(160), gif[14] = 0, gif[15] = 0;
  struct mx_pkt p;
  mx_pkt_begin(&p, pk, sizeof pk, MX_PKT_VIF);
  mx_pkt_cnt(&p, mx_vif_flushe(), mx_vif_stcycl(1, 1));
  mx_pkt_mpg(&p, kick_vu, (u32)(kick_vu_end - kick_vu), 0);
  mx_pkt_unpack(&p, MX_UNPACK_V4_32, 0, 4, 0, gif);
  mx_pkt_vif(&p, mx_vif_mscal(0));
  mx_pkt_vif(&p, mx_vif_flush());
  mx_pkt_end(&p, 0, 0);
  CHECK(mx_pkt_finish(&p) > 0);
  mx_pkt_writeback(&p);
  mx_dma_chain(MX_CH_VIF1, mx_pkt_start(&p), 1);
  CHECK(mx_dma_wait(MX_CH_VIF1) == 0);
  /* FINISH over PATH3 after PATH1 is done */
  mx_pkt_begin(&p, pk, sizeof pk, 0);
  mx_pkt_end(&p, 0, 0);
  mx_gs_finish_ad(&p);
  u32 f0 = mx_gs.finish;
  send(&p);
  CHECK(mx_gs_wait_finish(f0, 1) == 0);
  CHECK_EQ(pixel(310, 155), GREEN);
}

static void frames(void) {
  static u32 fpk[2][128] MX_ALIGNED(64);
  u32 v0 = mx_gs.vsync;
  for (u32 k = 0; k < 6; k++) {
    int b = mx_gs_frame_begin();
    struct mx_pkt p;
    mx_pkt_begin(&p, fpk[b], sizeof fpk[b], 0);
    mx_pkt_cnt(&p, 0, 0);
    mx_gs_draw_env(&p, b);
    mx_gs_clear(&p, k * 40, 0, 0);
    mx_gs_finish_ad(&p);
    mx_pkt_end(&p, 0, 0);
    CHECK(mx_pkt_finish(&p) > 0);
    mx_pkt_writeback(&p);
    mx_gs_frame_submit(mx_pkt_start(&p));
  }
  u32 t0 = mx_count();
  while (mx_gs.shown < 6 && mx_count() - t0 < 300000000u)
    ;
  CHECK_EQ(mx_gs.shown, 6);
  CHECK_EQ(mx_gs.shown_fbp, mx_gs.fbp[1]);
  mx_printf("frames: 6 frames in %u vsyncs\n", mx_gs.vsync - v0);
  CHECK(mx_gs.vsync - v0 >= 3);
}

#define STEP(x)                                                                            \
  do {                                                                                     \
    mx_printf("- %s\n", #x);                                                               \
    x;                                                                                     \
  } while (0)

int main(void) {
  mx_gs_init(640, 224);
  STEP(path3_triangle());
  STEP(signal_label());
  STEP(texture());
  STEP(path1_vu1());
  STEP(frames());
  return t_done("gs");
}
