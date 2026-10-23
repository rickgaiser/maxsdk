/* Phase 6: the render pipeline. Frames go through the MFIFO to VIF1; two
   lit meshes of 600 vertices each are drawn by the VU1 render kernel in
   double-buffered batches, and a quad is textured from a PATH3 upload.
   The framebuffer is read back and checked. */

#include "t.h"

extern const u8 render_vu[], render_vu_end[], dbl_vu[], dbl_vu_end[];

#define FRAMES 4
#define VERTS 600

static u8 ring[64 * 1024] MX_ALIGNED(64 * 1024);
static float mesh[2][VERTS][3][4] MX_ALIGNED(64);
static float quad[6][3][4] MX_ALIGNED(64);
static u32 tex[16 * 16] MX_ALIGNED(64);
static float hdr[3][7][4] MX_ALIGNED(16);
static u32 px[64] MX_ALIGNED(64);
static float cin[200][4] MX_ALIGNED(64), cout[200][4] MX_ALIGNED(64);

static const struct mx_kernel render = {.vu = 1,
                                        .code = render_vu,
                                        .code_bytes = 0, /* set in main */
                                        .entry = 0,
                                        .hdr_qw = 0,
                                        .count_qw = 0,
                                        .data_qw = 0,
                                        .item_qw = 3,
                                        .dbuf = 1,
                                        .out_qw = 3};

static void vert(float *v, float x, float y, float s, float t) {
  v[0] = x, v[1] = y, v[2] = 1000, v[3] = 1;   /* position */
  v[4] = 0, v[5] = 0, v[6] = 1, v[7] = 0;      /* normal */
  v[8] = s, v[9] = t, v[10] = 1, v[11] = 0;    /* st */
}

/* 200 triangles of 8 x 8 pixels in a 20 x 10 grid at (x0, y0) */
static void grid(float (*m)[3][4], float x0, float y0) {
  for (int i = 0; i < VERTS / 3; i++) {
    float x = x0 + (float)(i % 20) * 10, y = y0 + (float)(i / 20) * 10;
    vert(m[3 * i][0], x, y, 0, 0);
    vert(m[3 * i + 1][0], x + 8, y, 0, 0);
    vert(m[3 * i + 2][0], x, y + 8, 0, 0);
  }
}

/* window pixels -> GS coordinates (P = c0 x + c1 y + c2 z + c3) */
static void header(float h[7][4], float lr, float lg, float lb, float amb) {
  static const float m[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 4; j++)
      h[i][j] = m[i][j];
  h[3][0] = 2048 - 320;
  h[3][1] = 2048 - 112;
  h[4][0] = 0, h[4][1] = 0, h[4][2] = 1, h[4][3] = 0;
  h[5][0] = lr, h[5][1] = lg, h[5][2] = lb, h[5][3] = 0;
  h[6][0] = amb, h[6][1] = amb, h[6][2] = amb, h[6][3] = 128;
}

static u32 pixel(u32 fb, u32 x, u32 y) {
  mx_gs_download(px, MX_GS_PAGE(fb), mx_gs.fbw, MX_GS_PSM_CT32, x & ~31u, y, 32, 1);
  return px[x & 31];
}

int main(void) {
  mx_gs_init(640, 224);
  mx_vu_reset(2);
  struct mx_kernel k = render;
  k.code_bytes = (u32)(render_vu_end - render_vu);
  CHECK_EQ(mx_vu_load(1, k.code, k.code_bytes, 0), 1);
  mx_printf("frame: %u vertices per batch\n", mx_frame_vu1_max_items(&k, 7));
  CHECK(mx_frame_vu1_max_items(&k, 7) >= 80);

  grid(mesh[0], 20, 20);
  grid(mesh[1], 320, 20);
  /* a 32 x 32 quad at (100, 150), texture coordinates 0..1 */
  vert(quad[0][0], 100, 150, 0, 0);
  vert(quad[1][0], 132, 150, 1, 0);
  vert(quad[2][0], 100, 182, 0, 1);
  vert(quad[3][0], 132, 150, 1, 0);
  vert(quad[4][0], 132, 182, 1, 1);
  vert(quad[5][0], 100, 182, 0, 1);
  for (int i = 0; i < 16 * 16; i++)
    tex[i] = 0x80112233u;
  mx_dcache_writeback(mesh, sizeof mesh);
  mx_dcache_writeback(quad, sizeof quad);
  mx_dcache_writeback(tex, sizeof tex);
  header(hdr[0], 64, 32, 16, 16); /* (80, 48, 32) */
  header(hdr[1], 0, 100, 0, 8);   /* (8, 108, 8) */
  header(hdr[2], 0, 0, 0, 128);

  mx_frame_init(ring, sizeof ring, (void *)0x70002000, 0x2000);
  mx_frame_vu1_setup(&k);
  CHECK_EQ(mx_frame_texture(0, mx_gs.tbp, 1, MX_GS_PSM_CT32, 16, 16, tex), 0);
  const u64 tri = mx_giftag(0, 1, (s32)mx_gs_prim(MX_GS_PRIM_TRIANGLE, 0, 0, 0, 0, 0, 0, 0, 0),
                            MX_GIF_FLG_PACKED, 3);
  const u64 tri_tex = mx_giftag(0, 1, (s32)mx_gs_prim(MX_GS_PRIM_TRIANGLE, 0, 1, 0, 0, 0, 0, 0, 0),
                                MX_GIF_FLG_PACKED, 3);
  const u64 regs = mx_gif_regs3(MX_GIF_REG_ST, MX_GIF_REG_RGBAQ, MX_GIF_REG_XYZ2);
  u32 per = mx_frame_vu1_max_items(&k, 7) / 3 * 3;
  /* the doubling kernel at micro address 512, run between the two meshes */
  const struct mx_kernel dbl = {.vu = 1, .code = dbl_vu, .code_bytes = (u32)(dbl_vu_end - dbl_vu),
                                .entry = 512, .hdr_qw = 1, .count_qw = 0, .data_qw = 1,
                                .item_qw = 1};
  for (int i = 0; i < 200; i++)
    for (int j = 0; j < 4; j++)
      cin[i][j] = (float)(i * 4 + j);
  mx_dcache_writeback(cin, sizeof cin);
  const u32 chdr[4] MX_ALIGNED(16) = {0, 0, 0, 0};
  int cbad = 0;
  for (int f = 0; f < FRAMES; f++) {
    mx_frame_begin(0x10, 0x10, 0x10);
    for (int m = 0; m < 2; m++) {
      for (u32 v = 0; v < VERTS; v += per)
        mx_frame_vu1_batch(&k, tri, regs, hdr[m], 7, mesh[m][v], VERTS - v < per ? VERTS - v : per);
      if (m == 0) {
        for (int i = 0; i < 200; i++)
          cout[i][0] = 0;
        CHECK_EQ(mx_frame_vu1_compute(&dbl, chdr, cin, 200, cout, (void *)0x70000000, 0x2000), 0);
        for (int i = 0; i < 200; i++)
          for (int j = 0; j < 4; j++)
            cbad += cout[i][j] != 2 * cin[i][j];
      }
    }
    mx_frame_wait_path3();
    u64 *c = mx_frame_direct(3);
    c = mx_e_ad_tag(c, 2, 1);
    c = mx_e_ad(c, MX_GS_REG_TEX0_1, mx_gs_tex0(mx_gs.tbp, 1, MX_GS_PSM_CT32, 4, 4, 1, 1));
    c = mx_e_ad(c, MX_GS_REG_TEX1_1, mx_gs_tex1(0, 0, 0, 0, 0, 0, 0));
    mx_frame_commit(c);
    mx_frame_vu1_batch(&k, tri_tex, regs, hdr[2], 7, quad, 6);
    mx_frame_end();
  }
  if (cbad)
    mx_printf("frame: VU1 compute between passes: %d of %d values wrong\n", cbad, FRAMES * 800);
  CHECK_EQ(cbad, 0);
  u32 t0 = mx_count();
  while (mx_gs.shown < FRAMES && mx_count() - t0 < 300000000u)
    ;
  mx_frame_stop();
  mx_printf("frame: %u frames shown, %u pushes, missed %u, starved %u\n", mx_gs.shown,
            mx_frame_stats.pushes, mx_gs.missed, mx_gs.starved);
  CHECK_EQ(mx_gs.shown, FRAMES);

  u32 fb = mx_gs.fbp[(FRAMES - 1) & 1];
  /* inside triangle (2, 0) of each grid, in the first and the last batch */
  CHECK_EQ(pixel(fb, 20 + 20 + 2, 20 + 2), 0x80203050u);
  CHECK_EQ(pixel(fb, 20 + 190 + 2, 20 + 90 + 2), 0x80203050u);
  CHECK_EQ(pixel(fb, 320 + 20 + 2, 20 + 2), 0x80086c08u);
  CHECK_EQ(pixel(fb, 320 + 190 + 2, 20 + 90 + 2), 0x80086c08u);
  /* between two triangles: the clear colour */
  CHECK_EQ(pixel(fb, 20 + 9, 20 + 9) & 0xFFFFFFu, 0x101010u);
  /* the textured quad (decal) */
  CHECK_EQ(pixel(fb, 116, 166) & 0xFFFFFFu, 0x112233u);
  /* every triangle of both grids */
  int bad = 0;
  for (int m = 0; m < 2; m++)
    for (int i = 0; i < VERTS / 3; i++) {
      u32 x = (m ? 320 : 20) + (u32)(i % 20) * 10 + 2, y = 20 + (u32)(i / 20) * 10 + 2;
      bad += pixel(fb, x, y) != (m ? 0x80086c08u : 0x80203050u);
    }
  if (bad)
    mx_printf("frame: %d of %d triangles missing\n", bad, 2 * VERTS / 3);
  CHECK_EQ(bad, 0);
  return t_done("frame");
}
