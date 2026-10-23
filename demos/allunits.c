/* Phase 7 demo: every unit busy at once, from one EE thread through
   mx_sched (PLAN_MAXSDK.md 4.4):
   - VU1 transforms and lights the scene's cubes (render kernel, PATH1) and
     the GS draws them, fed by the frame pipeline (MFIFO -> VIF1);
   - PATH3 carries the texture the EE draws anew every frame;
   - VU0 micro mode runs a particle step over all particles (a VIF0 queue,
     a worker on a pool, polled from every wait);
   - the EE and its FPU compute the cube matrices, respawn the particles
     and project them into sprites (DIRECT, PATH2): frame f draws the
     particles of step f - 1 while VU0 computes step f.
   Prints the busy share of every unit (sampled), the frame statistics, and
   checks the VU0 step against the FPU and the last frame's pixels. */

#include <mx/mx.h>

#ifndef FRAMES
#define FRAMES 240
#endif
#define CUBES 64
#define P 960   /* particles */
#define PB 120  /* particles per VU0 batch: 2 + 2 * 120 quadwords of 256 */

extern const u8 render_vu[], render_vu_end[], particles_vu[], particles_vu_end[];

static u8 ring[128 * 1024] MX_ALIGNED(128 * 1024);
static float cube[36][3][4] MX_ALIGNED(64);
static u32 tex[2][64 * 64] MX_ALIGNED(64);
static float uni[7][4] MX_ALIGNED(16);
static float part[P][2][4] MX_ALIGNED(64); /* position, velocity */
static float phdr[2][4] MX_ALIGNED(16);
static u32 px[32] MX_ALIGNED(64);

static struct mx_kernel render = {.vu = 1, .code = render_vu, .item_qw = 3, .dbuf = 1, .out_qw = 3};
static struct mx_kernel particles = {
    .vu = 0, .code = particles_vu, .hdr_qw = 2, .count_qw = 0, .data_qw = 2, .item_qw = 2};

static const float DT = 1.0f / 60, G = 30;

/* ---- the scene (as demos/scene.c) ---------------------------------------- */

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

static void sincos(float a, float *s, float *c) {
  while (a > 3.14159265f)
    a -= 6.28318531f;
  while (a < -3.14159265f)
    a += 6.28318531f;
  float a2 = a * a;
  *s = a * (1 - a2 / 6 * (1 - a2 / 20 * (1 - a2 / 42 * (1 - a2 / 72))));
  *c = 1 - a2 / 2 * (1 - a2 / 12 * (1 - a2 / 30 * (1 - a2 / 56)));
}

static const float FX = 600, FY = 300, ZB = 1000000;

static void cube_uni(int i, u32 f) {
  float sx, cx, sy, cy;
  sincos(0.031f * (float)(s32)f + 0.7f * (float)i, &sx, &cx);
  sincos(0.023f * (float)(s32)f + 1.3f * (float)i, &sy, &cy);
  float r[3][3] = {{cy, 0, sy}, {sx * sy, cx, -sx * cy}, {-cx * sy, sx, cx * cy}};
  float m[3][4];
  for (int a = 0; a < 3; a++)
    for (int b = 0; b < 3; b++)
      m[a][b] = 1.2f * r[a][b];
  m[0][3] = ((float)(i % 8) - 3.5f) * 5.5f;
  m[1][3] = ((float)(i / 8) - 3.5f) * 3.4f;
  m[2][3] = 45;
  for (int j = 0; j < 4; j++) {
    uni[j][0] = FX * m[0][j] + 2048 * m[2][j];
    uni[j][1] = FY * m[1][j] + 2048 * m[2][j];
    uni[j][2] = j == 3 ? ZB : 0;
    uni[j][3] = m[2][j];
  }
  static const float L[3] = {0.2575f, -0.4292f, -0.8584f};
  for (int b = 0; b < 3; b++)
    uni[4][b] = r[0][b] * L[0] + r[1][b] * L[1] + r[2][b] * L[2];
  uni[4][3] = 0;
  uni[5][0] = uni[5][1] = uni[5][2] = 100, uni[5][3] = 0;
  uni[6][0] = uni[6][1] = uni[6][2] = 28, uni[6][3] = 128;
}

static void draw_texture(u32 *t, u32 f) {
  for (u32 y = 0; y < 64; y++)
    for (u32 x = 0; x < 64; x++) {
      u32 v = ((x * 4 + f * 3) ^ (y * 4 - f * 2)) & 0xFF;
      t[y * 64 + x] = 0x80000000u | v << 16 | (255 - v) << 8 | (v >> 1 | 0x40);
    }
  mx_dcache_writeback(t, 64 * 64 * 4);
}

/* ---- particles ----------------------------------------------------------- */

static u32 rnd_state = 12345;
static float rnd(void) { /* -1 .. 1 */
  rnd_state = rnd_state * 1664525u + 1013904223u;
  return (float)(s32)(rnd_state >> 8 & 0xFFFF) / 32768.0f - 1.0f;
}

static void respawn(float *p) {
  p[0] = rnd() * 2, p[1] = 6, p[2] = 30 + rnd() * 2, p[3] = 1;
  p[4] = rnd() * 6, p[5] = -22 - rnd() * 6, p[6] = rnd() * 3, p[7] = 0;
}

static struct mx_vuq q0;
static struct mx_pool ppool;
static struct mx_vu_worker vu0w;

static int step_done(void *arg) {
  (void)arg;
  return mx_pool_empty(&ppool) && !vu0w.n;
}

/* Sprites of the particles of the last step, by DIRECT: PACKED RGBAQ,
   XYZ2, XYZ2 per particle; dead ones are respawned for the next step. */
static void emit_particles(void) {
  const u64 tag = mx_giftag(0, 0, (s32)mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 0, 0, 0, 0, 0, 0, 0),
                            MX_GIF_FLG_PACKED, 3);
  const u64 regs = mx_gif_regs3(MX_GIF_REG_RGBAQ, MX_GIF_REG_XYZ2, MX_GIF_REG_XYZ2);
  for (u32 i0 = 0; i0 < P; i0 += 60) {
    u64 *c = mx_frame_direct(1 + 3 * 60), *t = c;
    c += 2;
    u32 n = 0;
    for (u32 i = i0; i < i0 + 60; i++) {
      float *p = part[i][0];
      if (p[1] > 14 || p[2] < 5) {
        respawn(p);
        continue;
      }
      float q = 1 / p[2];
      u32 x = (u32)(s32)((2048 + FX * p[0] * q) * 16), y = (u32)(s32)((2048 + FY * p[1] * q) * 16);
      u32 z = (u32)(s32)(ZB * 16 * q);
      c = mx_e_rgbaq(c, 0xFF, 0xE0, 0x40, 0x80);
      c = mx_e_xyz2(c, x - 24, y - 16, z, 0);
      c = mx_e_xyz2(c, x + 24, y + 16, z, 0);
      n++;
    }
    /* last packet of the DIRECT: EOP; unused room is filled with NOP tags */
    mx_e_giftag(t, n, 1, (s32)MX_GET(GIFTAG_PRIM, tag), MX_GIF_FLG_PACKED, 3, regs);
    while (c < t + 2 * (1 + 3 * 60))
      c = mx_e_giftag(c, 0, 1, -1, MX_GIF_FLG_PACKED, 0, 0);
    mx_frame_commit(c);
  }
  mx_dcache_writeback(part, sizeof part);
}

/* ---- one frame ----------------------------------------------------------- */

static struct mx_gs_regs regs_cache;
static u32 wait_step;

static void frame(u32 f) {
  mx_sched_frame_begin(0x18, 0x18, 0x30);
  int slot = (int)(f & 1);
  u32 tbp = mx_gs.tbp + 64 * (u32)slot;
  draw_texture(tex[slot], f);
  mx_frame_texture(slot, tbp, 1, MX_GS_PSM_CT32, 64, 64, tex[slot]);
  regs_cache.valid[MX_GS_REG_TEST_1] = 0;
  u64 *tag = mx_frame_reserve(6), *c = mx_gs_e_regs_begin(tag + 2);
  c = mx_gs_e_reg(c, &regs_cache, MX_GS_REG_TEST_1, mx_gs_test(0, 0, 0, 0, 0, 0, 1, 2));
  c = mx_gs_e_reg(c, &regs_cache, MX_GS_REG_TEX0_1, mx_gs_tex0(tbp, 1, MX_GS_PSM_CT32, 6, 6, 1, 0));
  c = mx_gs_e_reg(c, &regs_cache, MX_GS_REG_TEX1_1, mx_gs_tex1(0, 0, 1, 1, 0, 0, 0));
  c = mx_gs_e_regs_close(tag + 2, c, 1);
  if (c != tag + 2) {
    mx_e_cnt(tag, (u32)(c - tag) / 2 - 1, 0, mx_vif_direct((u32)(c - tag) / 2 - 1));
    mx_frame_commit(c);
  }
  mx_frame_wait_path3();
  const u64 giftag = mx_giftag(0, 1, (s32)mx_gs_prim(MX_GS_PRIM_TRIANGLE, 1, 1, 0, 0, 0, 0, 0, 0),
                               MX_GIF_FLG_PACKED, 3);
  const u64 regs = mx_gif_regs3(MX_GIF_REG_ST, MX_GIF_REG_RGBAQ, MX_GIF_REG_XYZ2);
  for (int i = 0; i < CUBES; i++) {
    cube_uni(i, f);
    mx_frame_vu1_batch(&render, giftag, regs, uni, 7, cube, 36);
    if ((i & 7) == 7)
      mx_sched_poll(); /* VU0 gets its next batch */
  }
  /* the particles of step f - 1, then start step f */
  u32 t0 = mx_count();
  mx_sched_wait(step_done, 0);
  wait_step += mx_count() - t0;
  emit_particles();
  mx_pool_init(&ppool, P);
  mx_frame_end();
}

static void wait_shown(u32 n) {
  u32 t0 = mx_count();
  while (mx_gs.shown < n && mx_count() - t0 < 300000000u)
    mx_sched_poll();
}

/* One VU0 step against the FPU (same formula; VU and FPU round alike for
   these few operations, allow a little). */
static int check_step(void) {
  static float before[P][2][4], ref[P][2][4];
  for (int i = 0; i < P; i++)
    for (int j = 0; j < 2; j++)
      for (int k = 0; k < 4; k++)
        before[i][j][k] = part[i][j][k];
  for (int i = 0; i < P; i++) {
    float vy = before[i][1][1] + phdr[1][1];
    ref[i][1][0] = before[i][1][0], ref[i][1][1] = vy, ref[i][1][2] = before[i][1][2];
    ref[i][0][0] = before[i][0][0] + before[i][1][0] * DT;
    ref[i][0][1] = before[i][0][1] + vy * DT;
    ref[i][0][2] = before[i][0][2] + before[i][1][2] * DT;
  }
  mx_pool_init(&ppool, P);
  mx_sched_wait(step_done, 0);
  int bad = 0;
  for (int i = 0; i < P; i++)
    for (int k = 0; k < 3; k++) {
      float d = part[i][0][k] - ref[i][0][k], e = part[i][1][k] - ref[i][1][k];
      bad += d > 1e-3f || d < -1e-3f || e > 1e-3f || e < -1e-3f;
    }
  return bad;
}

int main(void) {
  mx_gs_init(640, 224);
  mx_vu_reset(3);
  render.code_bytes = (u32)(render_vu_end - render_vu);
  particles.code_bytes = (u32)(particles_vu_end - particles_vu);
  mx_vu_load(1, render.code, render.code_bytes, 0);
  mx_vu_load(0, particles.code, particles.code_bytes, 0);
  make_cube();
  for (int i = 0; i < P; i++)
    respawn(part[i][0]);
  mx_dcache_writeback(part, sizeof part);
  phdr[1][0] = DT, phdr[1][1] = G * DT;
  mx_vuq_init(&q0, 0, 1);
  mx_vuq_readback_spr(&q0, (void *)0x70000000, 0x2000);
  mx_pool_init(&ppool, 0);
  mx_vu_worker_init(&vu0w, &q0, &particles, phdr, part, 0, &ppool, PB);
  mx_sched_add(&vu0w.task);
  mx_gs_regs_reset(&regs_cache);
  mx_frame_init(ring, sizeof ring, (void *)0x70002000, 0x2000);
  mx_frame_vu1_setup(&render);

  u32 busy[MX_TS_N];
  mx_trace_sample(2000);
  u32 v0 = mx_gs.vsync, t0 = mx_count();
  for (u32 f = 0; f < FRAMES; f++)
    frame(f);
  wait_shown(FRAMES);
  u32 cycles = mx_count() - t0, vs = mx_gs.vsync - v0;
  u32 ns = mx_trace_busy(busy);
  mx_trace_sample(0);
  mx_sched_wait(step_done, 0);
  mx_frame_stop();

  mx_printf("allunits: %u frames in %u vsyncs (%u.%03u s), missed %u, starved %u\n", FRAMES, vs,
            cycles / MX_EE_HZ, cycles % MX_EE_HZ / (MX_EE_HZ / 1000), mx_gs.missed,
            mx_gs.starved);
  mx_printf("allunits: EE per frame: wait for the back buffer %u, for ring space %u, for the "
            "particle step %u cycles\n",
            mx_frame_stats.wait_buffer / FRAMES, mx_frame_stats.wait_ring / FRAMES,
            wait_step / FRAMES);
  mx_printf("allunits: VU0 %u batches, %u particle updates\n", vu0w.batches, vu0w.items);
  static const char *units[MX_TS_N] = {"VIF0 DMA", "VIF1 DMA", "GIF DMA",  "fromSPR", "toSPR",
                                       "VU0",      "VU1",      "GIF path", "GS FIFO",
                                       "VU1 GIF wait"};
  for (int u = 0; u < MX_TS_N; u++)
    mx_printf("allunits: busy %-8s %3u%% of %u samples\n", units[u], ns ? busy[u] * 100 / ns : 0,
              ns);

  int fail = 0;
  int bad = check_step();
  mx_printf("allunits: VU0 step vs FPU: %d of %d values off\n", bad, 6 * P);
  fail += bad != 0;
  fail += mx_gs.shown < FRAMES;
  /* the last frame shows cubes and particles */
  u32 fb = mx_gs.fbp[(FRAMES - 1) & 1], cubes = 0, sparks = 0;
  for (u32 y = 40; y < 200; y += 8)
    for (u32 x = 0; x < 640; x += 32) {
      mx_gs_download(px, MX_GS_PAGE(fb), mx_gs.fbw, MX_GS_PSM_CT32, x, y, 32, 1);
      for (int i = 0; i < 32; i++) {
        u32 p = px[i] & 0xFFFFFF;
        sparks += p == 0x40E0FF;
        cubes += p != 0x301818 && p != 0x40E0FF;
      }
    }
  mx_printf("allunits: %u cube and %u particle pixels sampled\n", cubes, sparks);
  fail += cubes < 100 || sparks < 10;
  MX_TEST_END(fail);
  return fail;
}
