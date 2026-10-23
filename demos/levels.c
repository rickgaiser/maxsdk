/* PLAN_PROCGEN P1: the cost of each quality level (demos/lvl0-3.vuk) on the
   PS2. Terrain patches of 33 x 33 vertices, each one triangle strip (the rows
   joined by two repeated vertices, which make degenerate triangles), drawn
   by the frame pipeline without vsync. Two scenes:
     A. small triangles (about 2 pixels), 64 patches: the vertex rate;
     B. larger triangles (about 17 pixels), 16 patches, overdrawn: with fill.
   For each level and scene: EE cycles per frame and per vertex, and the busy
   share of VU1, the GIF path and the GS FIFO (sampled). */

#include <mx/mx.h>
#include "lvl0.h"
#include "lvl1.h"
#include "lvl2.h"
#include "lvl3.h"
#include "tf0.h"

extern const u8 lvl0_vu[], lvl0_vu_end[], lvl1_vu[], lvl1_vu_end[], lvl2_vu[], lvl2_vu_end[],
    lvl3_vu[], lvl3_vu_end[], tf0_vu[], tf0_vu_end[];

#define G 33                     /* vertices per side */
#define NV (2 * G * (G - 1) + 2 * (G - 2)) /* strip vertices of a patch */
#define FRAMES 20
#ifndef FIRST_LEVEL
#define FIRST_LEVEL 0
#endif

static u8 ring[64 * 1024] MX_ALIGNED(64 * 1024);
static float pos[NV + 8][3] MX_ALIGNED(16);
static s8 nrm[NV + 8][4] MX_ALIGNED(16);
static u32 col[NV + 8] MX_ALIGNED(16);
static s16 uv[NV + 8][2] MX_ALIGNED(16);
static u32 tex[64 * 64] MX_ALIGNED(64);
static float uni[16][4] MX_ALIGNED(16);

/* sin by a short series (no libm) */
static float sinf_(float a) {
  while (a > 3.14159265f)
    a -= 6.28318531f;
  while (a < -3.14159265f)
    a += 6.28318531f;
  float a2 = a * a;
  return a * (1 - a2 / 6 * (1 - a2 / 20 * (1 - a2 / 42 * (1 - a2 / 72))));
}
static float cosf_(float a) { return sinf_(a + 1.57079633f); }
static float rsqrt_(float l) {
  float r = 1;
  for (int k = 0; k < 10; k++)
    r = r * (1.5f - 0.5f * l * r * r);
  return r;
}

/* The patch: h = 3 sin(0.4 x) cos(0.3 z), in strip order (row r: (x, r),
   (x, r + 1) for each x; rows joined by repeating the last vertex of a row
   and the first of the next). */
static void make_patch(void) {
  u32 k = 0;
  for (int r = 0; r < G - 1; r++) {
    for (int x = 0; x < G; x++)
      for (int d = 0; d < 2; d++) {
        if (r && !x && !d) {
          /* the join: the last vertex again, then this one twice */
          pos[k][0] = pos[k - 1][0], pos[k][1] = pos[k - 1][1], pos[k][2] = pos[k - 1][2];
          nrm[k][0] = nrm[k - 1][0], nrm[k][1] = nrm[k - 1][1], nrm[k][2] = nrm[k - 1][2];
          col[k] = col[k - 1], uv[k][0] = uv[k - 1][0], uv[k][1] = uv[k - 1][1];
          k++;
        }
        int z = r + d;
        float fx = (float)x, fz = (float)z;
        float h = 3 * sinf_(0.4f * fx) * cosf_(0.3f * fz);
        float dx = 1.2f * cosf_(0.4f * fx) * cosf_(0.3f * fz);
        float dz = -0.9f * sinf_(0.4f * fx) * sinf_(0.3f * fz);
        float s = rsqrt_(dx * dx + 1 + dz * dz) * 127;
        pos[k][0] = fx, pos[k][1] = h, pos[k][2] = fz;
        nrm[k][0] = (s8)(s32)(-dx * s), nrm[k][1] = (s8)(s32)s, nrm[k][2] = (s8)(s32)(-dz * s);
        nrm[k][3] = 0;
        u32 g = (u32)(s32)(128 + 40 * h);
        col[k] = 0x80000000u | (g / 2) << 16 | g << 8 | (255 - g);
        uv[k][0] = (s16)(x * 512), uv[k][1] = (s16)(z * 512);
        k++;
        if (r && !x && !d) {
          pos[k][0] = pos[k - 1][0], pos[k][1] = pos[k - 1][1], pos[k][2] = pos[k - 1][2];
          nrm[k][0] = nrm[k - 1][0], nrm[k][1] = nrm[k - 1][1], nrm[k][2] = nrm[k - 1][2];
          col[k] = col[k - 1], uv[k][0] = uv[k - 1][0], uv[k][1] = uv[k - 1][1];
          k++;
        }
      }
  }
  mx_dcache_writeback(pos, sizeof pos);
  mx_dcache_writeback(nrm, sizeof nrm);
  mx_dcache_writeback(col, sizeof col);
  mx_dcache_writeback(uv, sizeof uv);
}

/* Patch i of a scene: tiles of tw x th pixels, layer after layer. */
static void patch_uniforms(int lvl, int i, u32 tw, u32 th) {
  u32 cols = 640 / tw, rows = 224 / th;
  u32 t = (u32)i % (cols * rows);
  float ox = (float)(s32)(2048 - 320 + (t % cols) * tw), oy = (float)(s32)(2048 - 112 + (t / cols) * th);
  float sx = (float)(s32)tw / (G - 1), sz = (float)(s32)th / (G - 1);
  for (int j = 0; j < 16; j++)
    for (int k = 0; k < 4; k++)
      uni[j][k] = 0;
  /* P = (ox + sx x, oy + sz z, Z, 1), Z = 100000 + 1000 h + 100 layer */
  uni[0][0] = sx, uni[1][2] = 1000, uni[2][1] = sz;
  uni[3][0] = ox, uni[3][1] = oy, uni[3][2] = (float)(s32)(100000 + 100 * (i / (s32)(cols * rows)));
  uni[3][3] = 1;
  if (lvl == 1 || lvl == 2) {
    uni[4][0] = 0.3f / 127, uni[4][1] = 0.9f / 127, uni[4][2] = 0.3f / 127;
    uni[5][0] = uni[5][1] = uni[5][2] = 0.8f;
    uni[6][0] = uni[6][1] = uni[6][2] = 0.25f, uni[6][3] = 1;
  } else if (lvl == 3) {
    static const float L[3][3] = {{0.3f, 0.9f, 0.3f}, {-0.7f, 0.5f, 0.2f}, {0.1f, 0.4f, -0.9f}};
    for (int j = 0; j < 3; j++)
      for (int k = 0; k < 3; k++)
        uni[4 + k][j] = L[j][k] / 127;
    uni[7][0] = 0.7f, uni[7][1] = 0.65f, uni[7][2] = 0.6f;
    uni[8][0] = 0.1f, uni[8][1] = 0.15f, uni[8][2] = 0.3f;
    uni[9][0] = 0.2f, uni[9][1] = 0.1f, uni[9][2] = 0.05f;
    uni[10][0] = uni[10][1] = uni[10][2] = 0.15f, uni[10][3] = 1;
    uni[11][0] = 0, uni[11][1] = 200;        /* constant light fog */
    uni[11][2] = 2048, uni[11][3] = 100000; /* nothing out of the guard band */
    uni[4][3] = -1000;                      /* nor too near */
  } else if (lvl == 4) {
    uni[4][0] = 0, uni[4][1] = 200; /* tf0 (procgen's far terrain): constant fog */
  }
  mx_dcache_writeback(uni, sizeof uni);
}

static void frame(int lvl, const struct mx_shader *s, int patches, u32 tw, u32 th) {
  mx_frame_begin(0x10, 0x18, 0x30);
  if (lvl == 2 || lvl == 3) {
    u64 *c = mx_frame_direct(6);
    c = mx_e_ad_tag(c, 5, 1);
    c = mx_e_ad(c, MX_GS_REG_TEX0_1, mx_gs_tex0(mx_gs.tbp, 1, MX_GS_PSM_CT32, 6, 6, 0, 0));
    c = mx_e_ad(c, MX_GS_REG_TEX1_1, mx_gs_tex1(0, 0, 1, 1, 0, 0, 0));
    c = mx_e_ad(c, MX_GS_REG_CLAMP_1, mx_gs_clamp(0, 0, 0, 0, 0, 0));
    c = mx_e_ad(c, MX_GS_REG_FOGCOL, 0x806040);
    c = mx_e_ad(c, MX_GS_REG_TEST_1, mx_gs_test(0, 0, 0, 0, 0, 0, 1, 1));
    mx_frame_commit(c);
  }
  u32 prim = (u32)mx_gs_prim(MX_GS_PRIM_TRISTRIP, 1, lvl == 2 || lvl == 3, lvl >= 3, 0, 0, 0, 0, 0);
  const void *all[4] = {pos, nrm, col, uv}, *streams[4];
  u32 k = 0;
  streams[k++] = all[0];
  if (s->nin >= 3)
    streams[k++] = all[1];
  streams[k++] = all[2];
  if (s->nin == 4)
    streams[k++] = all[3];
  for (int i = 0; i < patches; i++) {
    patch_uniforms(lvl, i, tw, th);
    mx_frame_shader_draw(s, prim, uni, streams, NV);
  }
  mx_frame_end();
}

int main(void) {
  mx_gs_init(640, 224);
  mx_vu_reset(2);
  make_patch();
  for (u32 i = 0; i < 64 * 64; i++)
    tex[i] = 0x80000000u | (((i ^ i >> 6) & 8) ? 0xC0C0C0u : 0x606060u);
  mx_dcache_writeback(tex, sizeof tex);
  mx_frame_init(ring, sizeof ring, (void *)0x70002000, 0x2000);
  mx_frame_texture(0, mx_gs.tbp, 1, MX_GS_PSM_CT32, 64, 64, tex);
  mx_frame_wait_path3();
  struct mx_shader sh[5] = {MX_VUK_SHADER(LVL0, lvl0_vu, lvl0_vu_end),
                            MX_VUK_SHADER(LVL1, lvl1_vu, lvl1_vu_end),
                            MX_VUK_SHADER(LVL2, lvl2_vu, lvl2_vu_end),
                            MX_VUK_SHADER(LVL3, lvl3_vu, lvl3_vu_end),
                            MX_VUK_SHADER(TF0, tf0_vu, tf0_vu_end)};
  static const struct {
    const char *name;
    int patches;
    u32 tw, th;
  } scenes[2] = {{"A small", 64, 80, 56}, {"B larger", 16, 320, 112}};
  static const char *units[MX_TS_N] = {"VIF0 DMA", "VIF1 DMA", "GIF DMA",  "fromSPR", "toSPR",
                                       "VU0",      "VU1",      "GIF path", "GS FIFO",
                                       "VU1 GIF wait"};
  mx_printf("levels: %u vertices, %u triangles (%u degenerate) per patch\n", NV, NV - 2,
            4 * (G - 2));
  mx_frame_vsync(0);
  /* level 4: procgen's tf0 (level 0 with fog) */
  for (int lvl = FIRST_LEVEL; lvl < 5; lvl++) {
    mx_frame_shader_setup(&sh[lvl]);
    for (int sc = 0; sc < 2; sc++) {
      u32 busy[MX_TS_N];
      u32 base = mx_gs.frames;
      frame(lvl, &sh[lvl], scenes[sc].patches, scenes[sc].tw, scenes[sc].th); /* warm up */
      u32 d0 = mx_count();
      while (mx_gs.frames_done < base + 1 && mx_count() - d0 < 300000000u)
        ;
      base = mx_gs.frames;
      mx_trace_sample(2000);
      u32 t0 = mx_count();
      for (int f = 0; f < FRAMES; f++) {
        frame(lvl, &sh[lvl], scenes[sc].patches, scenes[sc].tw, scenes[sc].th);
#ifdef DEBUG_HANG
        u32 w0 = mx_count();
        while (mx_gs.frames_done < base + f + 1 && mx_count() - w0 < 100000000u)
          ;
        if (mx_gs.frames_done < base + f + 1) {
          mx_printf("lvl%d frame %d stuck: VIF1_STAT %08x GIF_STAT %08x VPU_STAT %08x "
                    "VIF1 CHCR %08x MADR %08x TADR %08x done %u\n",
                    lvl, f, MX_R32(MX_VIF_VIF1_STAT), MX_R32(MX_GIF_STAT), mx_vpu_stat(),
                    MX_R32(0x10009000), MX_R32(0x10009010), MX_R32(0x10009030),
                    mx_gs.frames_done - base);
          MX_TEST_END(1);
          return 1;
        }
#endif
      }
      d0 = mx_count();
      while (mx_gs.frames_done < base + FRAMES && mx_count() - d0 < 300000000u)
        ;
      u32 cycles = (mx_count() - t0) / FRAMES;
      u32 ns = mx_trace_busy(busy);
      mx_trace_sample(0);
      u32 verts = (u32)scenes[sc].patches * NV;
      mx_printf("lvl%d %s: %u vertices, %u cycles/frame, %u.%02u cycles/vertex, %u.%u fps;", lvl,
                scenes[sc].name, verts, cycles, cycles / verts, cycles * 100 / verts % 100,
                MX_EE_HZ / cycles, MX_EE_HZ * 10 / cycles % 10);
      for (int u = 6; u < MX_TS_N; u++)
        mx_printf(" %s %u%%", units[u], ns ? busy[u] * 100 / ns : 0);
      mx_printf("\n");
    }
  }
  mx_frame_vsync(1);

  /* one frame of level 2, scene A, shown; then the patches must cover the
     screen (the background is 0x301810) */
  u32 last = mx_gs.frames;
  mx_frame_shader_setup(&sh[2]);
  frame(2, &sh[2], 64, 80, 56);
  u32 d0 = mx_count();
  while (mx_gs.shown < last + 1 && mx_count() - d0 < 300000000u)
    ;
  mx_frame_stop();
  static u32 px[64] MX_ALIGNED(64);
  u32 covered = 0;
  for (u32 x = 0; x < 640; x += 64) {
    mx_gs_download(px, MX_GS_PAGE(mx_gs.fbp[last & 1]), mx_gs.fbw, MX_GS_PSM_CT32, x, 100, 64, 1);
    for (int i = 0; i < 64; i++)
      covered += (px[i] & 0xFFFFFF) != 0x301810;
  }
  mx_printf("levels: %u of 640 pixels of line 100 drawn\n", covered);
  if (mx_gs_save_tga((int)(last & 1), "host:levels.tga") == 0)
    mx_printf("levels: frame saved to levels.tga\n");
  int fail = covered < 600;
  MX_TEST_END(fail);
  return fail;
}
