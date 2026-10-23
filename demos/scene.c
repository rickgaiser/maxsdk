/* Phase 6 demo: textured, lit, perspective cubes drawn by the VU1 render
   kernel through the frame pipeline (MFIFO -> VIF1 -> VU1 -> XGKICK), with
   a texture that the EE draws anew every frame and streams over PATH3.

   Three runs:
   1. FRAMES frames at one per vsync: shown / missed / starved vblanks, how
      long the EE waits, and the busy share of every unit (sampled);
   2. the same scene without vsync: EE cycles per frame, i.e. the
      pipeline's throughput, with double-buffered VU1 batches;
   3. as 2 with a FLUSHE after every batch (single buffering), to compare.
   Then the last frame is checked by reading pixels back. */

#include <mx/mx.h>

#ifndef FRAMES
#define FRAMES 240
#endif
#define MAX_CUBES 256

extern const u8 render_vu[], render_vu_end[];

static u8 ring[64 * 1024] MX_ALIGNED(64 * 1024);
static float cube[36][3][4] MX_ALIGNED(64);
static u32 tex[2][64 * 64] MX_ALIGNED(64);
static float uni[7][4] MX_ALIGNED(16);
static u32 px[32] MX_ALIGNED(64);

static struct mx_kernel render = {.vu = 1, .code = render_vu, .item_qw = 3, .dbuf = 1, .out_qw = 3};

/* ---- the cube and the math ------------------------------------------------- */

static void make_cube(void) {
  static const int axes[6][3][3] = {
      {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}},   {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
      {{0, 1, 0}, {0, 0, 1}, {1, 0, 0}},   {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
      {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},   {{0, 0, -1}, {0, 1, 0}, {1, 0, 0}}};
  static const int corner[6][2] = {{0, 0}, {1, 0}, {0, 1}, {0, 1}, {1, 0}, {1, 1}};
  for (int f = 0; f < 6; f++)
    for (int k = 0; k < 6; k++) {
      float *v = cube[6 * f + k][0];
      float a = corner[k][0] ? 1.0f : -1.0f, b = corner[k][1] ? 1.0f : -1.0f;
      for (int i = 0; i < 3; i++) {
        v[i] = (float)axes[f][0][i] + a * (float)axes[f][1][i] + b * (float)axes[f][2][i];
        v[4 + i] = (float)axes[f][0][i];
      }
      v[3] = 1, v[7] = 0;
      v[8] = (float)corner[k][0], v[9] = (float)corner[k][1], v[10] = 1, v[11] = 0;
    }
  mx_dcache_writeback(cube, sizeof cube);
}

/* sin/cos by a short series (no libm) */
static void sincos(float a, float *s, float *c) {
  while (a > 3.14159265f)
    a -= 6.28318531f;
  while (a < -3.14159265f)
    a += 6.28318531f;
  float a2 = a * a;
  *s = a * (1 - a2 / 6 * (1 - a2 / 20 * (1 - a2 / 42 * (1 - a2 / 72))));
  *c = 1 - a2 / 2 * (1 - a2 / 12 * (1 - a2 / 30 * (1 - a2 / 56)));
}

/* The constants of cube i in frame f: MVP columns, light in object space,
   light colour, ambient. */
/* (float)(s32)f: clang expands an unsigned -> float conversion with an add
   that is not marked finite, which becomes a soft-float call (__addsf3). */
static void cube_uni(int i, int n, u32 f) {
  float sx, cx, sy, cy;
  sincos(0.031f * (float)(s32)f + 0.7f * (float)i, &sx, &cx);
  sincos(0.023f * (float)(s32)f + 1.3f * (float)i, &sy, &cy);
  /* R = Rx * Ry */
  float r[3][3] = {{cy, 0, sy}, {sx * sy, cx, -sx * cy}, {-cx * sy, sx, cx * cy}};
  int cols = n <= 64 ? 8 : 16;
  float tx = ((float)(i % cols) - (float)(cols - 1) / 2) * (cols == 8 ? 5.5f : 2.9f);
  float ty = ((float)(i / cols) - (float)((n + cols - 1) / cols - 1) / 2) * (cols == 8 ? 3.4f : 1.7f);
  float tz = cols == 8 ? 45 : 48, s = cols == 8 ? 1.2f : 0.6f;
  const float fx = 600, fy = 300, B = 1000000;
  /* model rows: M = [s R | t] */
  float m[3][4];
  for (int a = 0; a < 3; a++) {
    for (int b = 0; b < 3; b++)
      m[a][b] = s * r[a][b];
  }
  m[0][3] = tx, m[1][3] = ty, m[2][3] = tz;
  for (int j = 0; j < 4; j++) {
    uni[j][0] = fx * m[0][j] + 2048 * m[2][j];
    uni[j][1] = fy * m[1][j] + 2048 * m[2][j];
    uni[j][2] = j == 3 ? B : 0;
    uni[j][3] = m[2][j];
  }
  /* towards the light (0.3, -0.5, -1) normalised, into object space: R^T L */
  static const float L[3] = {0.2575f, -0.4292f, -0.8584f};
  for (int b = 0; b < 3; b++)
    uni[4][b] = r[0][b] * L[0] + r[1][b] * L[1] + r[2][b] * L[2];
  uni[4][3] = 0;
  uni[5][0] = uni[5][1] = uni[5][2] = 100, uni[5][3] = 0;
  uni[6][0] = uni[6][1] = uni[6][2] = 28, uni[6][3] = 128;
}

/* The EE's texture for frame f. */
static void draw_texture(u32 *t, u32 f) {
  for (u32 y = 0; y < 64; y++)
    for (u32 x = 0; x < 64; x++) {
      u32 v = ((x * 4 + f * 3) ^ (y * 4 - f * 2)) & 0xFF;
      t[y * 64 + x] = 0x80000000u | v << 16 | (255 - v) << 8 | (v >> 1 | 0x40);
    }
  mx_dcache_writeback(t, 64 * 64 * 4);
}

/* ---- one frame -------------------------------------------------------------- */

static struct mx_gs_regs regs_cache;
static u32 build_cycles;

static void frame(u32 f, int n, int flushe) {
  mx_frame_begin(0x18, 0x18, 0x30);
  u32 t0 = mx_count();
  int slot = (int)(f & 1);
  u32 tbp = mx_gs.tbp + 64 * (u32)slot;
  draw_texture(tex[slot], f);
  mx_frame_texture(slot, tbp, 1, MX_GS_PSM_CT32, 64, 64, tex[slot]);
  /* the drawing state, only what changed (the clear rewrote TEST) */
  regs_cache.valid[MX_GS_REG_TEST_1] = 0;
  u64 *tag = mx_frame_reserve(6), *c = mx_gs_e_regs_begin(tag + 2);
  c = mx_gs_e_reg(c, &regs_cache, MX_GS_REG_TEST_1, mx_gs_test(0, 0, 0, 0, 0, 0, 1, 2));
  c = mx_gs_e_reg(c, &regs_cache, MX_GS_REG_TEX0_1,
                  mx_gs_tex0(tbp, 1, MX_GS_PSM_CT32, 6, 6, 1, 0));
  c = mx_gs_e_reg(c, &regs_cache, MX_GS_REG_TEX1_1, mx_gs_tex1(0, 0, 1, 1, 0, 0, 0));
  c = mx_gs_e_reg(c, &regs_cache, MX_GS_REG_CLAMP_1, mx_gs_clamp(0, 0, 0, 0, 0, 0));
  c = mx_gs_e_regs_close(tag + 2, c, 1);
  if (c != tag + 2) {
    mx_e_cnt(tag, (u32)(c - tag) / 2 - 1, 0, mx_vif_direct((u32)(c - tag) / 2 - 1));
    mx_frame_commit(c);
  }
  mx_frame_wait_path3(); /* this frame's texture is in GS memory */
  const u64 giftag = mx_giftag(0, 1, (s32)mx_gs_prim(MX_GS_PRIM_TRIANGLE, 1, 1, 0, 0, 0, 0, 0, 0),
                               MX_GIF_FLG_PACKED, 3);
  const u64 regs = mx_gif_regs3(MX_GIF_REG_ST, MX_GIF_REG_RGBAQ, MX_GIF_REG_XYZ2);
  for (int i = 0; i < n; i++) {
    cube_uni(i, n, f);
    mx_frame_vu1_batch(&render, giftag, regs, uni, 7, cube, 36);
    if (flushe)
      mx_frame_commit(mx_e_cnt(mx_frame_reserve(1), 0, mx_vif_flushe(), 0));
  }
  build_cycles += mx_count() - t0;
  mx_frame_end();
}

static void wait_shown(u32 n) {
  u32 t0 = mx_count();
  while (mx_gs.shown < n && mx_count() - t0 < 300000000u)
    ;
}

int main(void) {
  mx_gs_init(640, 224);
  mx_vu_reset(2);
  render.code_bytes = (u32)(render_vu_end - render_vu);
  mx_vu_load(1, render.code, render.code_bytes, 0);
  make_cube();
  mx_gs_regs_reset(&regs_cache);
  mx_frame_init(ring, sizeof ring, (void *)0x70002000, 0x2000);
  mx_frame_vu1_setup(&render);
  int fail = 0;

  /* 1. one frame per vsync, 64 cubes */
  u32 busy[MX_TS_N];
  mx_trace_sample(2000);
  u32 v0 = mx_gs.vsync, t0 = mx_count();
  build_cycles = 0;
  for (u32 f = 0; f < FRAMES; f++)
    frame(f, 64, 0);
  wait_shown(FRAMES);
  u32 cycles = mx_count() - t0, vs = mx_gs.vsync - v0;
  u32 ns = mx_trace_busy(busy);
  mx_trace_sample(0);
  mx_printf("scene: %u frames in %u vsyncs (%u.%03u s), missed %u, starved %u\n", FRAMES, vs,
            cycles / MX_EE_HZ, cycles % MX_EE_HZ / (MX_EE_HZ / 1000), mx_gs.missed, mx_gs.starved);
  mx_printf("scene: EE per frame: build %u, wait for the back buffer %u, wait for ring space %u cycles\n",
            build_cycles / FRAMES, mx_frame_stats.wait_buffer / FRAMES,
            mx_frame_stats.wait_ring / FRAMES);
  static const char *units[MX_TS_N] = {"VIF0 DMA", "VIF1 DMA", "GIF DMA",  "fromSPR", "toSPR",
                                       "VU0",      "VU1",      "GIF path", "GS FIFO",
                                       "VU1 GIF wait"};
  for (int u = 0; u < MX_TS_N; u++)
    if (u != 0 && u != 5)
      mx_printf("scene: busy %-8s %3u%% of %u samples\n", units[u], ns ? busy[u] * 100 / ns : 0, ns);
  fail += mx_gs.shown - (mx_gs.frames - FRAMES) < FRAMES;

  /* 2. and 3. throughput without vsync, 256 cubes */
  mx_frame_vsync(0);
  for (int flushe = 0; flushe < 2; flushe++) {
    u32 base = mx_gs.frames, n = 60;
    t0 = mx_count();
    for (u32 f = 0; f < n; f++)
      frame(base + f, MAX_CUBES, flushe);
    u32 d0 = mx_count();
    while (mx_gs.frames_done < base + n && mx_count() - d0 < 300000000u)
      ;
    cycles = mx_count() - t0;
    mx_printf("scene: no vsync, %d cubes (%d triangles), %s: %u cycles per frame (%u.%u fps)\n",
              MAX_CUBES, MAX_CUBES * 12, flushe ? "FLUSHE after every batch" : "double buffered",
              cycles / n, MX_EE_HZ / (cycles / n), MX_EE_HZ * 10 / (cycles / n) % 10);
  }
  mx_frame_vsync(1);

  /* 4. one more frame with 64 cubes, then check it */
  u32 last = mx_gs.frames;
  frame(last, 64, 0);
  wait_shown(last + 1);
  mx_frame_stop();
  u32 fb = mx_gs.fbp[last & 1];
  /* the centre of the screen lies between cubes; the centre of cube 27
     (row 3, column 3 of 8) is textured and lit */
  mx_gs_download(px, MX_GS_PAGE(fb), mx_gs.fbw, MX_GS_PSM_CT32, 0, 112, 32, 1);
  u32 bg = px[0];
  int cubes = 0;
  for (u32 x = 32; x < 608; x += 32) {
    mx_gs_download(px, MX_GS_PAGE(fb), mx_gs.fbw, MX_GS_PSM_CT32, x, 112 - 10, 32, 1);
    for (int i = 0; i < 32; i++)
      cubes += (px[i] & 0xFFFFFF) != (bg & 0xFFFFFF);
  }
  mx_printf("scene: background %08x, %d of 576 pixels on row 102 are cubes\n", bg, cubes);
  fail += (bg & 0xFFFFFF) != 0x301818 || cubes < 50;
  MX_TEST_END(fail);
  return fail;
}
