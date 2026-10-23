/* A spinning textured triangle, drawn twice per frame through the frame
   pipeline (phase 6; the phase 3 version built and waited for every packet
   itself): once as a GIF packet by DIRECT over PATH2 (left), once by the VU1
   render kernel over PATH1 (right). The texture goes over PATH3 once. Runs
   FRAMES frames at one per vsync, then checks that every frame was shown
   and that both triangles are textured. */

#include <mx/mx.h>

#ifndef FRAMES
#define FRAMES 120
#endif

extern const u8 render_vu[], render_vu_end[];

static u8 ring[16 * 1024] MX_ALIGNED(16 * 1024);
static u32 tex[16 * 16] MX_ALIGNED(64);
static float tri[2][3][3][4] MX_ALIGNED(64); /* per frame slot: 3 vertices */
static float uni[7][4] MX_ALIGNED(16);

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
  mx_vu_reset(2);
  struct mx_kernel k = {.vu = 1, .code = render_vu, .item_qw = 3, .dbuf = 1, .out_qw = 3};
  k.code_bytes = (u32)(render_vu_end - render_vu);
  mx_vu_load(1, k.code, k.code_bytes, 0);
  for (u32 y = 0; y < 16; y++)
    for (u32 x = 0; x < 16; x++)
      tex[y * 16 + x] = ((x ^ y) & 4) ? 0x80FFFFFFu : 0x80FF4000u;
  mx_dcache_writeback(tex, sizeof tex);
  /* the render kernel's constants: window pixels to GS coordinates, no
     lighting (ambient 128) */
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 4; j++)
      uni[i][j] = i == j ? 1.0f : 0.0f;
  uni[3][0] = 2048 - 320, uni[3][1] = 2048 - 112;
  uni[6][0] = uni[6][1] = uni[6][2] = 128, uni[6][3] = 128;

  mx_frame_init(ring, sizeof ring, (void *)0x70002000, 0x2000);
  mx_frame_vu1_setup(&k);
  mx_frame_texture(0, mx_gs.tbp, 1, MX_GS_PSM_CT32, 16, 16, tex);
  const u64 tex0 = mx_gs_tex0(mx_gs.tbp, 1, MX_GS_PSM_CT32, 4, 4, 1, 1);
  const u64 prim = mx_gs_prim(MX_GS_PRIM_TRIANGLE, 0, 1, 0, 0, 0, 1, 0, 0);

  float c = 1, s = 0;
  const float d = 0.05f; /* radians per frame */
  u32 t0 = mx_count(), v0 = mx_gs.vsync;
  for (u32 f = 0; f < FRAMES; f++) {
    mx_frame_begin(0x20, 0x20, 0x40);
    struct v2 v[3];
    if (f == 0)
      mx_frame_wait_path3();

    /* left: a GIF packet by DIRECT (PATH2) */
    corners(v, c, s, 180, 112, 90);
    u64 *p = mx_frame_direct(3 + 1 + 6);
    p = mx_e_ad_tag(p, 3 + 6, 1);
    p = mx_e_ad(p, MX_GS_REG_RGBAQ, mx_gs_rgbaq(0x80, 0x80, 0x80, 0x80, 1.0f));
    p = mx_e_ad(p, MX_GS_REG_TEX0_1, tex0);
    p = mx_e_ad(p, MX_GS_REG_PRIM, prim);
    for (int i = 0; i < 3; i++) {
      p = mx_e_ad(p, MX_GS_REG_UV, mx_gs_uv(uvs[i][0], uvs[i][1]));
      p = mx_e_ad(p, MX_GS_REG_XYZ2, mx_gs_xyz(fx(v[i].x), fy(v[i].y), 0));
    }
    mx_frame_commit(p);

    /* right: the VU1 render kernel (PATH1), perspective ST with w = 1; the
       vertices are in this frame slot's array, free again two frames on */
    corners(v, c, -s, 460, 112, 90);
    float(*t)[3][4] = tri[f & 1];
    for (int i = 0; i < 3; i++) {
      float *q = t[i][0];
      q[0] = v[i].x, q[1] = v[i].y, q[2] = 0, q[3] = 1;
      q[4] = 0, q[5] = 0, q[6] = 1, q[7] = 0;
      q[8] = (float)uvs[i][0] / 256.0f, q[9] = (float)uvs[i][1] / 256.0f, q[10] = 1, q[11] = 0;
    }
    mx_dcache_writeback(t, sizeof tri[0]);
    mx_frame_vu1_batch(&k, mx_giftag(0, 1, (s32)mx_gs_prim(MX_GS_PRIM_TRIANGLE, 0, 1, 0, 0, 0, 0, 0, 0),
                                     MX_GIF_FLG_PACKED, 3),
                       mx_gif_regs3(MX_GIF_REG_ST, MX_GIF_REG_RGBAQ, MX_GIF_REG_XYZ2), uni, 7, t, 3);
    mx_frame_end();

    /* rotate */
    float c2 = c - s * d, s2 = s + c * d;
    float n = 1.5f - 0.5f * (c2 * c2 + s2 * s2); /* keep the length 1 */
    c = c2 * n;
    s = s2 * n;
  }
  u32 w0 = mx_count();
  while (mx_gs.shown < FRAMES && mx_count() - w0 < 300000000u)
    ;
  u32 cycles = mx_count() - t0, vs = mx_gs.vsync - v0;
  mx_frame_stop();
  mx_printf("spin: %u frames shown in %u vsyncs, missed %u, starved %u, %u.%03u s\n", mx_gs.shown,
            vs, mx_gs.missed, mx_gs.starved, cycles / MX_EE_HZ,
            cycles % MX_EE_HZ / (MX_EE_HZ / 1000));
  /* the centres of both triangles show the texture, not the clear color */
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
