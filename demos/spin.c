/* Phase 3 demo: a spinning textured triangle, double buffered, drawn once
   over PATH3 (left) and once by VU1 XGKICK over PATH1 (right). Runs FRAMES
   frames, then checks that every frame was shown. */

#include <mx/mx.h>

#ifndef FRAMES
#define FRAMES 120
#endif

extern const u8 kick_vu[], kick_vu_end[];

static u32 tex[16 * 16] MX_ALIGNED(64);
static u32 gif_pk[2][256] MX_ALIGNED(64);
static u32 vif_pk[2][256] MX_ALIGNED(64);
static u32 fin_pk[2][16] MX_ALIGNED(64);
static u32 vu_tri[2][4 * 8] MX_ALIGNED(64);

struct v2 {
  float x, y;
};

/* The three corners of the triangle around (cx, cy), radius r. */
static void corners(struct v2 *v, float c, float s, float cx, float cy, float r) {
  static const struct v2 base[3] = {{0, -1}, {0.866f, 0.5f}, {-0.866f, 0.5f}};
  for (int i = 0; i < 3; i++) {
    v[i].x = cx + r * (base[i].x * c - base[i].y * s);
    v[i].y = cy + r * (base[i].x * s + base[i].y * c);
  }
}

static u32 fx(float x) { return mx_gs_px(0) + (u32)(x * 16.0f); }
static u32 fy(float y) { return mx_gs_py(0) + (u32)(y * 16.0f); }

static const u32 uvs[3][2] = {{8 << 4, 0}, {16 << 4, 16 << 4}, {0, 16 << 4}};

int main(void) {
  mx_gs_init(640, 224);
  MX_R32(MX_VIF_VIF1_FBRST) = 1;
  for (u32 y = 0; y < 16; y++)
    for (u32 x = 0; x < 16; x++)
      tex[y * 16 + x] = ((x ^ y) & 4) ? 0x80FFFFFFu : 0x80FF4000u;
  mx_dcache_writeback(tex, sizeof tex);

  /* texture upload and the VU1 program, once */
  struct mx_pkt p;
  mx_pkt_begin(&p, gif_pk[0], sizeof gif_pk[0], 0);
  mx_pkt_cnt(&p, 0, 0);
  mx_gs_upload(&p, mx_gs.tbp, 1, MX_GS_PSM_CT32, 0, 0, 16, 16, tex);
  mx_pkt_end(&p, 0, 0);
  mx_pkt_finish(&p);
  mx_pkt_writeback(&p);
  mx_gs_send(gif_pk[0]);
  mx_pkt_begin(&p, vif_pk[0], sizeof vif_pk[0], MX_PKT_VIF);
  mx_pkt_end(&p, mx_vif_stcycl(1, 1), 0);
  mx_pkt_mpg(&p, kick_vu, (u32)(kick_vu_end - kick_vu), 0);
  mx_pkt_finish(&p);
  mx_pkt_writeback(&p);
  mx_dma_chain(MX_CH_VIF1, vif_pk[0], 1);
  mx_dma_wait(MX_CH_VIF1);

  float c = 1, s = 0;
  const float d = 0.05f; /* radians per frame */
  u32 t0 = mx_count();
  for (u32 f = 0; f < FRAMES; f++) {
    int b = mx_gs_frame_begin();
    struct v2 v[3];

    /* left: PATH3 */
    mx_pkt_begin(&p, gif_pk[b], sizeof gif_pk[b], 0);
    mx_pkt_cnt(&p, 0, 0);
    mx_gs_draw_env(&p, b);
    mx_gs_clear(&p, 0x20, 0x20, 0x40);
    corners(v, c, s, 180, 112, 90);
    mx_pkt_ad_begin(&p, 3 + 6, 1);
    mx_pkt_ad(&p, MX_GS_REG_RGBAQ, mx_gs_rgbaq(0x80, 0x80, 0x80, 0x80, 1.0f));
    mx_pkt_ad(&p, MX_GS_REG_TEX0_1, mx_gs_tex0(mx_gs.tbp, 1, MX_GS_PSM_CT32, 4, 4, 1, 1));
    mx_pkt_ad(&p, MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_TRIANGLE, 0, 1, 0, 0, 0, 1, 0, 0));
    for (int i = 0; i < 3; i++) {
      mx_pkt_ad(&p, MX_GS_REG_UV, mx_gs_uv(uvs[i][0], uvs[i][1]));
      mx_pkt_ad(&p, MX_GS_REG_XYZ2, mx_gs_xyz(fx(v[i].x), fy(v[i].y), 0));
    }
    mx_pkt_end(&p, 0, 0);
    mx_pkt_finish(&p);
    mx_pkt_writeback(&p);
    mx_gs_send(gif_pk[b]);

    /* right: a GIF packet unpacked into VU1 memory, sent by XGKICK */
    corners(v, c, -s, 460, 112, 90);
    u64 *g = (u64 *)vu_tri[b];
    g[0] = mx_giftag(1, 1, (s32)mx_gs_prim(MX_GS_PRIM_TRIANGLE, 0, 1, 0, 0, 0, 1, 0, 0),
                     MX_GIF_FLG_REGLIST, 6);
    g[1] = (u64)MX_GIF_REG_UV | (u64)MX_GIF_REG_XYZ2 << 4 | (u64)MX_GIF_REG_UV << 8 |
           (u64)MX_GIF_REG_XYZ2 << 12 | (u64)MX_GIF_REG_UV << 16 | (u64)MX_GIF_REG_XYZ2 << 20;
    for (int i = 0; i < 3; i++) {
      g[2 + 2 * i] = mx_gs_uv(uvs[i][0], uvs[i][1]);
      g[3 + 2 * i] = mx_gs_xyz(fx(v[i].x), fy(v[i].y), 0);
    }
    mx_pkt_begin(&p, vif_pk[b], sizeof vif_pk[b], MX_PKT_VIF);
    mx_pkt_end(&p, mx_vif_stcycl(1, 1), 0);
    mx_pkt_unpack(&p, MX_UNPACK_V4_32, 0, 4, 0, vu_tri[b]);
    mx_pkt_vif(&p, mx_vif_mscal(0));
    mx_pkt_vif(&p, mx_vif_flush());
    mx_pkt_finish(&p);
    mx_pkt_writeback(&p);
    mx_dma_chain(MX_CH_VIF1, vif_pk[b], 1);
    mx_dma_wait(MX_CH_VIF1);

    /* FINISH ends the frame; the vblank handler shows it */
    mx_pkt_begin(&p, fin_pk[b], sizeof fin_pk[b], 0);
    mx_pkt_end(&p, 0, 0);
    mx_gs_finish_ad(&p);
    mx_pkt_finish(&p);
    mx_pkt_writeback(&p);
    mx_gs_frame_submit(fin_pk[b]);

    /* rotate */
    float c2 = c - s * d, s2 = s + c * d;
    float n = 1.5f - 0.5f * (c2 * c2 + s2 * s2); /* keep the length 1 */
    c = c2 * n;
    s = s2 * n;
  }
  u32 w0 = mx_count();
  while (mx_gs.shown < FRAMES && mx_count() - w0 < 300000000u)
    ;
  u32 cycles = mx_count() - t0;
  mx_printf("spin: %u frames shown, %u vsyncs, %u.%03u s\n", mx_gs.shown, mx_gs.vsync,
            cycles / MX_EE_HZ, cycles % MX_EE_HZ / (MX_EE_HZ / 1000));
  /* the centers of both triangles show the texture, not the clear color */
  static u32 px[32] MX_ALIGNED(64);
  u32 fb = mx_gs.fbp[(FRAMES - 1) & 1];
  mx_gs_download(px, MX_GS_PAGE(fb), mx_gs.fbw, MX_GS_PSM_CT32, 160, 112, 32, 1);
  u32 left = px[20];
  mx_gs_download(px, MX_GS_PAGE(fb), mx_gs.fbw, MX_GS_PSM_CT32, 448, 112, 32, 1);
  u32 right = px[12];
  mx_printf("spin: centre pixels %08x %08x\n", left, right);
  int textured = (left == 0x80FFFFFFu || left == 0x80FF4000u) &&
                 (right == 0x80FFFFFFu || right == 0x80FF4000u);
  int ok = mx_gs.shown == FRAMES && textured;
  MX_TEST_END(ok ? 0 : 1);
  return ok ? 0 : 1;
}
