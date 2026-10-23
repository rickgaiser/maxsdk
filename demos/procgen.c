/* PLAN_PROCGEN P4: a flight over generated terrain, every unit at work, at
   640 x 448 and 60 fields a second.
     - VU0 makes the terrain heights (demos/fbm.vuk), chunk after chunk, as
       the camera needs them (the chunks in the view cone), and moves the
       dust (demos/dust.vuk), through a VIF0 queue;
     - the EE builds each chunk's vertex streams (normals, colour by height
       and slope, texture coordinates, a half resolution level and the edges
       that fill its cracks, trees) after a frame is queued, culls the chunks
       and chooses a shader for each by its distance;
     - VU1 draws: tf0-tf2 and lvl3 (terrain, fogged, the near one clipped by
       ADC), water (waves made on VU1), tree and dustdraw (sprites);
     - the GS draws, with a generated detail texture, tree texture and font.
   Two presets: "looks" (shaders by distance, the view distance kept so that
   every frame is ready well before its vblank) and "max count" (everything
   at level 0 and full resolution, the largest view, no vsync: the vertex
   rate).
   The run: PROCGEN_SECONDS of "looks" (frames, missed vblanks, units' busy
   share every 10 s), then 600 frames of "max count", then screenshots of
   both (host:procgen_looks.tga, host:procgen_max.tga).
   START on the pad (port 0) during "looks" switches to the interactive mode
   until TRIANGLE: SELECT cycles the quality (AUTO by distance, L0-L3 for all
   chunks, HALF: half resolution), START switches vsync (looks / max count),
   L1 and R1 set the view distance, SQUARE gives it back to the budget. */

#include <mx/mx.h>
#include <mx/pad.h>
#include "fbm.h"
#include "lvl3.h"
#include "tf0.h"
#include "tf1.h"
#include "tf2.h"
#include "tree.h"
#include "water.h"
#include "dust.h"
#include "dustdraw.h"

#ifndef PROCGEN_SECONDS
#define PROCGEN_SECONDS 120
#endif

extern const u8 fbm_vu[], fbm_vu_end[], tf0_vu[], tf0_vu_end[], tf1_vu[], tf1_vu_end[],
    tf2_vu[], tf2_vu_end[], lvl3_vu[], lvl3_vu_end[], water_vu[], water_vu_end[], tree_vu[],
    tree_vu_end[], dust_vu[], dust_vu_end[], dustdraw_vu[], dustdraw_vu_end[];

/* ---- world ---------------------------------------------------------------- */

#define CELLS 32
#define GV (CELLS + 1) /* vertices per side */
#define GB (GV + 2)    /* heights per side, with a border for the normals */
#define CELL 4.0f
#define CHUNK (CELLS * CELL)
#define NV_FULL (2 * GV * (GV - 1) + 2 * (GV - 2))
#define GH (CELLS / 2 + 1)
#define NV_HALF (2 * GH * (GH - 1) + 2 * (GH - 2))
/* An edge of a half resolution chunk next to a full resolution one: the
   full edge has a vertex between each two of the half edge, which leaves a
   crack (a T-junction). The triangles (a, m, b) of the half edge's vertices a,
   b and the full edge's m between them fill it exactly; one strip per edge:
   a0 m0 a1 a1 m1 a2 a2 ... m15 a16 (the repeated vertex joins them). */
#define NV_EDGE (3 * (GH - 1))
/* water: one flat grid for all chunks (the waves come from the chunk's
   origin), coarse, and fine for the chunks next to the eye, whose triangles
   near the eye are clipped away */
#define WG 9
#define WGF GV
#define NV_GRID(g) (2 * (g) * ((g) - 1) + 2 * ((g) - 2))
#define MAX_TREES 48
#define NCHUNK 192
#define WATER_LEVEL 0.0f
#define MAX_R 14

struct chunk {
  int cx, cz, state; /* state: 0 free, 1 queued, 2 generating, 3 ready */
  u32 used;          /* frame */
  float hmin, hmax;
  int water, ntrees;
};
enum { C_FREE, C_QUEUED, C_GEN, C_READY };

static struct chunk ch[NCHUNK];
static float heights[NCHUNK][GB * GB];
/* the vertex streams of each chunk: 8 spare vertices for batch rounding,
   and a multiple of 4 vertices, so that every chunk's stream starts on a
   quadword (12-byte positions) */
#define SLOTS(n) (((n) + 8 + 3) & ~3)
static float fpos[NCHUNK][SLOTS(NV_FULL)][3] MX_ALIGNED(16);
static s8 fnrm[NCHUNK][SLOTS(NV_FULL)][4] MX_ALIGNED(16);
static u32 fcol[NCHUNK][SLOTS(NV_FULL)] MX_ALIGNED(16);
static s16 fuv[NCHUNK][SLOTS(NV_FULL)][2] MX_ALIGNED(16);
static float hpos[NCHUNK][SLOTS(NV_HALF)][3] MX_ALIGNED(16);
static u32 hcol[NCHUNK][SLOTS(NV_HALF)] MX_ALIGNED(16);
static float epos[NCHUNK][4][SLOTS(NV_EDGE)][3] MX_ALIGNED(16);
static u32 ecol[NCHUNK][4][SLOTS(NV_EDGE)] MX_ALIGNED(16);
static float wpos[SLOTS(NV_GRID(WG))][3] MX_ALIGNED(16);
static float wfine[SLOTS(NV_GRID(WGF))][3] MX_ALIGNED(16);
static float trees[NCHUNK][SLOTS(MAX_TREES)][3] MX_ALIGNED(16);

/* ---- small math (no libm) ------------------------------------------------- */

static float sinf_(float a) {
  while (a > 3.14159265f)
    a -= 6.28318531f;
  while (a < -3.14159265f)
    a += 6.28318531f;
  float a2 = a * a;
  return a * (1 - a2 / 6 * (1 - a2 / 20 * (1 - a2 / 42 * (1 - a2 / 72))));
}
static float cosf_(float a) { return sinf_(a + 1.57079633f); }
static float sqrtf_(float v) { return __builtin_sqrtf(v); }
static float absf(float v) { return v < 0 ? -v : v; }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float floorf_(float v) {
  float t = (float)(s32)v;
  return t > v ? t - 1 : t;
}
static float itof(int v) { return (float)(s32)v; }
static u32 hash32(u32 v) {
  v ^= v >> 16;
  v *= 0x7feb352du;
  v ^= v >> 15;
  v *= 0x846ca68bu;
  return v ^ (v >> 16);
}

/* ---- terrain heights on VU0 ------------------------------------------------- */

#define NSAMP (GB * GB)
#define NOISE_SCALE (1.0f / 160.0f)
#define OCT 6

static float samp[NSAMP + 256][4] MX_ALIGNED(64);
static float sout[NSAMP + 256][4] MX_ALIGNED(64);
static u32 fbm_hdr[8] MX_ALIGNED(16);
static struct mx_kernel fbm_k;
static struct mx_vuq q0;
static struct {
  int c;      /* chunk, -1: none */
  int build_c; /* chunk whose heights are made, to be built by gen_build; -1: none */
  int kick;    /* the next batch is to be sent */
  int hold;    /* gen_poll does not send it (gen_kick will) */
  u32 at, n;  /* the batch in flight */
  mx_event e;
  u32 per;
  u32 cycles; /* EE cycles from the first batch to the heights */
  u32 build, build_max; /* EE cycles of the last and the longest build_chunk */
  u32 t0;
} gen = {.c = -1, .build_c = -1};

/* The height of the terrain from the noise value e (about -0.7..0.7):
   valleys flattened, mountains sharpened. */
static float shape(float e) {
  float m = e + 0.12f;
  return m > 0 ? 95 * m * m + 18 * m : 30 * m;
}

static void gen_start(int c) {
  float x0 = itof(ch[c].cx) * CHUNK - CELL, z0 = itof(ch[c].cz) * CHUNK - CELL;
  for (int j = 0; j < GB; j++)
    for (int i = 0; i < GB; i++) {
      float *s = samp[j * GB + i];
      s[0] = (x0 + itof(i) * CELL) * NOISE_SCALE + 300.5f;
      s[1] = (z0 + itof(j) * CELL) * NOISE_SCALE + 200.25f;
      s[2] = s[3] = 0;
    }
  mx_dcache_writeback(samp, sizeof(float) * 4 * NSAMP);
  gen.c = c;
  gen.at = 0;
  gen.n = NSAMP < gen.per ? NSAMP : gen.per;
  gen.t0 = mx_count();
  gen.e = mx_vuq_batch(&q0, &fbm_k, fbm_hdr, samp[0], gen.n);
  ch[c].state = C_GEN;
}

static void build_chunk(int c);
static void dust_update(void);

/* Called often: moves the generation on VU0 along (cheap). The streams of a
   chunk whose heights are made are built by gen_build. */
static void gen_kick(void) {
  if (gen.kick) {
    gen.kick = 0;
    gen.e = mx_vuq_batch(&q0, &fbm_k, fbm_hdr, samp[gen.at], gen.n);
  }
}

static int gen_poll(void) {
  if (gen.c < 0 || gen.kick || mx_vuq_done(&q0, gen.e) != 1)
    return 0;
#ifdef PROCGEN_DEBUG
  static int dbg_n;
  if (dbg_n < 12)
    mx_printf("gen: batch at %u done\n", gen.at);
#endif
  mx_vuq_wait(&q0, gen.e);
#ifdef PROCGEN_DEBUG
  if (dbg_n < 12)
    mx_printf("gen: waited\n");
#endif
  mx_vuq_read(&q0, &fbm_k, sout[gen.at], gen.n);
#ifdef PROCGEN_DEBUG
  if (dbg_n++ < 12)
    mx_printf("gen: read\n");
#endif
  gen.at += gen.n;
  if (gen.at < NSAMP) {
    gen.n = NSAMP - gen.at < gen.per ? NSAMP - gen.at : gen.per;
    gen.kick = 1;
    if (!gen.hold)
      gen_kick();
    return 0;
  }
  int c = gen.c;
  gen.c = -1;
  float *h = heights[c];
  for (int k = 0; k < NSAMP; k++)
    h[k] = shape(sout[k][2]);
#ifdef PROCGEN_DEBUG
  mx_printf("gen: heights of chunk %d\n", c);
#endif
  gen.build_c = c;
  return 0;
}

/* Builds the streams of the chunk whose heights are made (about 1.7 ms of
   EE time): called after a frame is queued, so that it overlaps with VU1 and
   the GS drawing it. Returns 1 when it built one. */
static int gen_build(void) {
  int c = gen.build_c;
  if (c < 0)
    return 0;
  u32 b0 = mx_count();
  build_chunk(c);
  gen.build = mx_count() - b0;
  gen.build_max = gen.build > gen.build_max ? gen.build : gen.build_max;
#ifdef PROCGEN_DEBUG
  mx_printf("gen: built\n");
#endif
  gen.build_c = -1;
  gen.cycles = mx_count() - gen.t0;
  return 1;
}

/* ---- dust on VU0 ----------------------------------------------------------- */

/* Motes in box units (demos/dust.vuk), moved by VU0 once a frame through the
   VIF0 queue of the heights, between their batches (VU0's data memory holds
   one batch). fbm and dust do not fit VU0's 4 KiB of micro memory together:
   both are at 0, and the queue sends the code of a batch's kernel with it
   when the other one was there. */
#define NDUST 1536
#define DUST_BOX 48.0f
static float dust[NDUST + 8][4] MX_ALIGNED(64);
static u32 dust_hdr[8] MX_ALIGNED(16);
static struct mx_kernel dust_k;
static int dust_pending; /* this frame's update is not done yet */
static u32 dust_vsync;   /* the vblank of the last update */

static void dust_init(void) {
  for (int i = 0; i < NDUST; i++) {
    u32 h = hash32((u32)i * 2654435761u + 7);
    dust[i][0] = itof((s32)(h & 1023)) / 1024, dust[i][1] = itof((s32)(h >> 10 & 1023)) / 1024;
    dust[i][2] = itof((s32)(h >> 20 & 1023)) / 1024, dust[i][3] = itof((s32)(h >> 7 & 255)) / 40;
  }
  mx_dcache_writeback(dust, sizeof dust);
  dust_k = MX_VUK_KERNEL(DUST, 0, dust_vu, dust_vu_end);
  dust_vsync = mx_gs.vsync;
}

/* The update of this frame, if VU0's queue is free (no batch of heights in
   flight): batch after batch, each read back in place. */
static void dust_update(void) {
  if (!dust_pending)
    return;
  dust_pending = 0;
  u32 v = mx_gs.vsync, dv = v - dust_vsync;
  dust_vsync = v;
  float dt = itof((s32)(dv > 4 ? 4 : dv)) / 60, *c = (float *)&dust_hdr[4 * VUK_DUST_C];
  c[0] = dt * 2.5f / DUST_BOX, c[1] = itof((s32)v) / 60, c[2] = 1.4f, c[3] = dt * 0.4f / DUST_BOX;
  u32 per = mx_kernel_max_items(&dust_k);
  for (u32 at = 0; at < NDUST; at += per) {
    u32 n = NDUST - at < per ? NDUST - at : per;
    mx_event e = mx_vuq_batch(&q0, &dust_k, dust_hdr, dust[at], n);
    mx_vuq_wait(&q0, e);
    mx_vuq_read(&q0, &dust_k, dust[at], n);
  }
  mx_dcache_writeback(dust, sizeof dust);
}

/* ---- chunk streams (EE) ---------------------------------------------------- */

static float smooth01(float e0, float e1, float v) {
  float t = clampf((v - e0) / (e1 - e0), 0, 1);
  return t * t * (3 - 2 * t);
}

/* Terrain colour by height h, slope s (0 flat .. 1 vertical) and a little
   per-vertex variation r (0..1). */
static u32 terrain_colour(float h, float s, float r) {
  float sand[3] = {178, 164, 118}, grass[3] = {64, 104, 44}, rock[3] = {112, 104, 96},
        snow[3] = {228, 230, 236}, c[3];
  float g = smooth01(0.5f, 3.0f, h), rk = smooth01(0.28f, 0.45f, s),
        sn = smooth01(52, 62, h + 25 * r - 12 * s);
  for (int k = 0; k < 3; k++) {
    float v = sand[k] + (grass[k] * (0.85f + 0.3f * r) - sand[k]) * g;
    v = v + (rock[k] - v) * rk;
    c[k] = v + (snow[k] - v) * sn;
  }
  return 0x80000000u | (u32)(s32)c[2] << 16 | (u32)(s32)c[1] << 8 | (u32)(s32)c[0];
}

static void build_chunk(int c) {
  const float *H = heights[c];
  struct chunk *k = &ch[c];
  static u32 gcol[GV * GV];
  static s8 gnrm[GV * GV][4];
  k->hmin = 1e9f, k->hmax = -1e9f;
  for (int j = 0; j < GV; j++)
    for (int i = 0; i < GV; i++) {
      float h = H[(j + 1) * GB + i + 1];
      float dx = H[(j + 1) * GB + i + 2] - H[(j + 1) * GB + i];
      float dz = H[(j + 2) * GB + i + 1] - H[j * GB + i + 1];
      float nx = -dx, ny = 2 * CELL, nz = -dz, l = 127 / sqrtf_(nx * nx + ny * ny + nz * nz);
      s8 *n = gnrm[j * GV + i];
      n[0] = (s8)(s32)(nx * l), n[1] = (s8)(s32)(ny * l), n[2] = (s8)(s32)(nz * l), n[3] = 0;
      u32 r = hash32((u32)(k->cx * 977 + i) * 131 + (u32)(k->cz * 613 + j));
      gcol[j * GV + i] = terrain_colour(h, 1 - ny * l / 127, itof((s32)(r & 255)) / 255);
      k->hmin = h < k->hmin ? h : k->hmin;
      k->hmax = h > k->hmax ? h : k->hmax;
    }
  /* full resolution: one strip, rows joined by repeating a vertex twice */
  u32 n = 0;
  for (int r = 0; r < GV - 1; r++)
    for (int x = 0; x < GV; x++)
      for (int d = 0; d < 2; d++) {
        int reps = (r && !x && !d) || (r < GV - 2 && x == GV - 1 && d) ? 2 : 1;
        int z = r + d, g = z * GV + x;
        for (int t = 0; t < reps; t++, n++) {
          fpos[c][n][0] = itof(x) * CELL, fpos[c][n][1] = H[(z + 1) * GB + x + 1];
          fpos[c][n][2] = itof(z) * CELL;
          for (int m = 0; m < 4; m++)
            fnrm[c][n][m] = gnrm[g][m];
          fcol[c][n] = gcol[g];
          fuv[c][n][0] = (s16)(x * 1024 > 32767 ? 32767 : x * 1024);
          fuv[c][n][1] = (s16)(z * 1024 > 32767 ? 32767 : z * 1024);
        }
      }
  /* half resolution, the even vertices */
  n = 0;
  for (int r = 0; r < GH - 1; r++)
    for (int x = 0; x < GH; x++)
      for (int d = 0; d < 2; d++) {
        int reps = (r && !x && !d) || (r < GH - 2 && x == GH - 1 && d) ? 2 : 1;
        int z = 2 * (r + d), xx = 2 * x;
        for (int t = 0; t < reps; t++, n++) {
          hpos[c][n][0] = itof(xx) * CELL, hpos[c][n][1] = H[(z + 1) * GB + xx + 1];
          hpos[c][n][2] = itof(z) * CELL;
          hcol[c][n] = gcol[z * GV + xx];
        }
      }
  /* the edges that fill the cracks: -z, +z, -x, +x */
  for (int e = 0; e < 4; e++) {
    n = 0;
    for (int g = 0; g <= CELLS; g++)
      for (int t = 0; t < (g && (g & 1) == 0 && g < CELLS ? 2 : 1); t++, n++) {
        int x = e < 2 ? g : e == 2 ? 0 : CELLS, z = e < 2 ? (e ? CELLS : 0) : g;
        epos[c][e][n][0] = itof(x) * CELL, epos[c][e][n][1] = H[(z + 1) * GB + x + 1];
        epos[c][e][n][2] = itof(z) * CELL;
        ecol[c][e][n] = gcol[z * GV + x];
      }
  }
  mx_dcache_writeback(epos[c], sizeof epos[c]);
  mx_dcache_writeback(ecol[c], sizeof ecol[c]);
  /* water over the chunk where it is below the water level */
  k->water = k->hmin < WATER_LEVEL;
  /* trees on gentle grass slopes */
  k->ntrees = 0;
  for (int j = 1; j < GV - 1 && k->ntrees < MAX_TREES; j += 2)
    for (int i = 1; i < GV - 1 && k->ntrees < MAX_TREES; i += 2) {
      u32 r = hash32((u32)(k->cx * 4099 + i) * 2053 + (u32)(k->cz * 8191 + j));
      float h = H[(j + 1) * GB + i + 1];
      float ny = gnrm[j * GV + i][1];
      if ((r & 255) < 40 && h > 3 && h < 40 && ny > 118) {
        float *t = trees[c][k->ntrees++];
        t[0] = itof(i) * CELL + itof((s32)(r >> 8 & 3)) - 1.5f;
        t[1] = h - 0.3f;
        t[2] = itof(j) * CELL + itof((s32)(r >> 12 & 3)) - 1.5f;
      }
    }
  mx_dcache_writeback(fpos[c], sizeof fpos[c]);
  mx_dcache_writeback(fnrm[c], sizeof fnrm[c]);
  mx_dcache_writeback(fcol[c], sizeof fcol[c]);
  mx_dcache_writeback(fuv[c], sizeof fuv[c]);
  mx_dcache_writeback(hpos[c], sizeof hpos[c]);
  mx_dcache_writeback(hcol[c], sizeof hcol[c]);
  mx_dcache_writeback(trees[c], sizeof trees[c]);
  k->state = C_READY;
}

/* The terrain height at world (x, z) from a ready chunk, or -1e9. */
static float terrain_height(float x, float z);

static int find_chunk(int cx, int cz) {
  for (int c = 0; c < NCHUNK; c++)
    if (ch[c].state != C_FREE && ch[c].cx == cx && ch[c].cz == cz)
      return c;
  return -1;
}

static float terrain_height(float x, float z) {
  int cx = (int)(s32)floorf_(x / CHUNK), cz = (int)(s32)floorf_(z / CHUNK);
  int c = find_chunk(cx, cz);
  if (c < 0 || ch[c].state != C_READY)
    return -1e9f;
  float fx = (x - itof(cx) * CHUNK) / CELL, fz = (z - itof(cz) * CHUNK) / CELL;
  int i = (int)(s32)fx, j = (int)(s32)fz;
  float u = fx - itof(i), v = fz - itof(j);
  const float *H = heights[c];
  float h00 = H[(j + 1) * GB + i + 1], h10 = H[(j + 1) * GB + i + 2];
  float h01 = H[(j + 2) * GB + i + 1], h11 = H[(j + 2) * GB + i + 2];
  return (h00 + (h10 - h00) * u) + ((h01 + (h11 - h01) * u) - (h00 + (h10 - h00) * u)) * v;
}

/* ---- camera ---------------------------------------------------------------- */

static struct {
  float x, y, z, yaw, pitch;
  float r[3], u[3], f[3]; /* right, up, forward */
} cam;
/* the screen: 640 x 448 interlaced, every frame drawn in full at 60 fps */
#define SW 640
#define SH 448
static const float FX = 457, FY = 457 * 0.467f * SH / 224, ZK = 1000000;

static void camera_update(float t) {
  /* a gently winding path */
  float s = 40 * t;
  cam.x = s;
  cam.z = 300 * sinf_(s * 0.0021f) + 140 * sinf_(s * 0.0057f);
  float dzds = 300 * 0.0021f * cosf_(s * 0.0021f) + 140 * 0.0057f * cosf_(s * 0.0057f);
  /* heading along the path: atan2(1, dz/ds) by a series is not needed, the
     direction vector is used directly */
  float l = sqrtf_(1 + dzds * dzds);
  float sy = 1 / l, cy = dzds / l; /* sin and cos of the yaw (x east, z north) */
  /* the highest ground ahead, nearer ground counting more */
  float h = terrain_height(cam.x, cam.z);
  for (int k = 1; k <= 8; k++) {
    float a = terrain_height(cam.x + itof(k) * 25 * sy, cam.z + itof(k) * 25 * cy) -
              itof(k) * 2.5f;
    h = a > h ? a : h;
  }
  float want = (h < WATER_LEVEL ? WATER_LEVEL : h) + 30;
  /* up quickly, down slowly */
  cam.y = cam.y == 0 ? want : cam.y + (want - cam.y) * (want > cam.y ? 0.12f : 0.02f);
  float sp = 0.22f, cp = 0.9759f; /* pitch down 12.7 degrees */
  cam.f[0] = sy * cp, cam.f[1] = -sp, cam.f[2] = cy * cp;
  cam.r[0] = cy, cam.r[1] = 0, cam.r[2] = -sy;
  cam.u[0] = sy * sp, cam.u[1] = cp, cam.u[2] = cy * sp;
}

/* The MVP columns of a chunk at world origin (ox, 0, oz): P = c0 x + c1 y +
   c2 z + c3 with P / P.w the GS coordinates and P.z / P.w = ZK / depth. */
static void chunk_mvp(float m[4][4], float ox, float oz) {
  float d[3] = {ox - cam.x, -cam.y, oz - cam.z};
  for (int a = 0; a < 3; a++) {
    m[a][0] = 2048 * cam.f[a] + FX * cam.r[a];
    m[a][1] = 2048 * cam.f[a] - FY * cam.u[a];
    m[a][2] = 0;
    m[a][3] = cam.f[a];
  }
  for (int k = 0; k < 4; k++)
    m[3][k] = m[0][k] * d[0] + m[1][k] * d[1] + m[2][k] * d[2];
  m[3][2] = ZK;
}

/* Visibility of a chunk: 0 culled, 1 inside the guard band, 2 needs
   clipping. */
static int chunk_visible(const struct chunk *k) {
  float x0 = itof(k->cx) * CHUNK, z0 = itof(k->cz) * CHUNK;
  int out[6] = {0}, guard = 1;
  for (int n = 0; n < 8; n++) {
    float p[3] = {x0 + (n & 1 ? CHUNK : 0) - cam.x,
                  (n & 2 ? k->hmax + 12 : (k->hmin < WATER_LEVEL ? k->hmin : WATER_LEVEL) - 1) -
                      cam.y,
                  z0 + (n & 4 ? CHUNK : 0) - cam.z};
    float xv = p[0] * cam.r[0] + p[1] * cam.r[1] + p[2] * cam.r[2];
    float yv = p[0] * cam.u[0] + p[1] * cam.u[1] + p[2] * cam.u[2];
    float zv = p[0] * cam.f[0] + p[1] * cam.f[1] + p[2] * cam.f[2];
    out[0] += zv < 1;
    out[1] += FX * xv > 330 * zv;
    out[2] += FX * xv < -330 * zv;
    out[3] += FY * yv > (SH / 2 + 8) * zv;
    out[4] += FY * yv < -(SH / 2 + 8) * zv;
    if (zv < 1.5f || absf(FX * xv) > 1900 * zv || absf(FY * yv) > 1900 * zv)
      guard = 0;
  }
  for (int i = 0; i < 5; i++)
    if (out[i] == 8)
      return 0;
  return guard ? 1 : 2;
}

/* ---- textures and font ------------------------------------------------------ */

static u32 detail[64 * 64] MX_ALIGNED(64);
static u32 treetex[32 * 64] MX_ALIGNED(64);
static u32 fonttex[256 * 8] MX_ALIGNED(64);
static u32 tbp_detail, tbp_tree, tbp_font;

/* 5x7 glyphs, one byte per row (bits 4..0, left to right) */
static const char glyph_chars[] = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ.:%/-";
static const u8 glyphs[][7] = {
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

/* Noise for the textures on the EE: value noise with a period (tiles). */
static float tile_noise(float x, float y, int period) {
  int ix = (int)(s32)floorf_(x), iy = (int)(s32)floorf_(y);
  float fx = x - itof(ix), fy = y - itof(iy);
  float sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy), v[4];
  for (int k = 0; k < 4; k++) {
    u32 cx = (u32)((ix + (k & 1)) % period + period) % (u32)period;
    u32 cy = (u32)((iy + (k >> 1)) % period + period) % (u32)period;
    v[k] = itof((s32)(hash32(cx * 73856093u ^ cy * 19349663u) & 1023)) / 1023;
  }
  float a = v[0] + (v[1] - v[0]) * sx, b = v[2] + (v[3] - v[2]) * sx;
  return a + (b - a) * sy;
}

static void make_textures(void) {
  /* detail: grey around 128 (modulate keeps the vertex colour), tiling */
  for (int y = 0; y < 64; y++)
    for (int x = 0; x < 64; x++) {
      float v = 0, a = 0.5f;
      for (int o = 0, p = 4; o < 4; o++, p *= 2, a *= 0.5f)
        v += a * tile_noise(itof(x) * itof(p) / 64, itof(y) * itof(p) / 64, p);
      u32 g = (u32)(s32)(96 + 72 * v);
      detail[y * 64 + x] = 0x80000000u | g << 16 | g << 8 | g;
    }
  /* tree: a trunk and a cone of dark green blobs; alpha 0 around it */
  for (int y = 0; y < 64; y++)
    for (int x = 0; x < 32; x++) {
      float dx = itof(x) - 15.5f, cone = itof(y) * 0.27f + 1;
      u32 px = 0;
      if (y > 54 && absf(dx) < 1.8f)
        px = 0x80000000u | 30 << 16 | 50 << 8 | 70;
      else if (y < 56 && absf(dx) < cone) {
        float n = tile_noise(itof(x) * 0.4f, itof(y) * 0.4f, 64);
        if (n > 0.18f + 0.5f * absf(dx) / cone) {
          u32 g = (u32)(s32)(40 + 70 * n);
          px = 0x80000000u | (g / 3) << 16 | g << 8 | (g / 3);
        }
      }
      treetex[y * 32 + x] = px;
    }
  /* font: glyph i at x = 6 i */
  for (u32 i = 0; i < 256 * 8; i++)
    fonttex[i] = 0;
  for (u32 g = 0; g < sizeof glyphs / sizeof *glyphs; g++)
    for (int r = 0; r < 7; r++)
      for (int b = 0; b < 5; b++)
        if (glyphs[g][r] >> (4 - b) & 1)
          fonttex[r * 256 + g * 6 + (u32)b] = 0x80FFFFFFu;
  mx_dcache_writeback(detail, sizeof detail);
  mx_dcache_writeback(treetex, sizeof treetex);
  mx_dcache_writeback(fonttex, sizeof fonttex);
  tbp_detail = mx_gs.tbp;
  tbp_tree = tbp_detail + 64;      /* 64 x 64 x 4 bytes = 64 blocks */
  tbp_font = tbp_tree + 64;        /* 32 x 64 x 4 = 32 blocks, aligned up */
  mx_frame_texture(0, tbp_detail, 1, MX_GS_PSM_CT32, 64, 64, detail);
  mx_frame_wait_path3();
  mx_frame_texture(1, tbp_tree, 1, MX_GS_PSM_CT32, 32, 64, treetex);
  mx_frame_wait_path3();
  mx_frame_texture(0, tbp_font, 4, MX_GS_PSM_CT32, 256, 8, fonttex);
  mx_frame_wait_path3();
}

/* ---- drawing ----------------------------------------------------------------- */

static float uni[16][4] MX_ALIGNED(16);
static const u32 SKY_TOP = 0x804a2a14u, HORIZON = 0x80c8a890u; /* ABGR */
static struct mx_shader sh_tf0, sh_tf1, sh_tf2, sh_l3, sh_water, sh_tree, sh_dust;
static const struct mx_shader *bound;

static void bind(const struct mx_shader *s) {
  if (bound != s) {
    mx_frame_shader_setup(s);
    bound = s;
  }
}

static u64 *gs_regs(u64 *c, u32 n) { return mx_e_ad_tag(c, n, 1); }

static void draw_sky(void) {
  /* the horizon line of the view: where the forward pitch puts y = 0 */
  float yh = 2048 - FY * (cam.f[1] / 0.9759f); /* tan(pitch) * FY above the centre */
  u32 hy = (u32)(s32)clampf(yh - (2048 - SH / 2), 0, SH);
  u64 *c = mx_frame_direct(12);
  c = gs_regs(c, 11);
  c = mx_e_ad(c, MX_GS_REG_TEST_1, mx_gs_test(0, 0, 0, 0, 0, 0, 1, 1));
  c = mx_e_ad(c, MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_TRISTRIP, 1, 0, 0, 0, 0, 0, 0, 0));
  u32 col[2] = {SKY_TOP, HORIZON};
  for (int v = 0; v < 4; v++) {
    u32 cl = col[v >> 1];
    c = mx_e_ad(c, MX_GS_REG_RGBAQ,
                mx_gs_rgbaq(cl & 0xFF, cl >> 8 & 0xFF, cl >> 16 & 0xFF, 0x80, 1.0f));
    c = mx_e_ad(c, MX_GS_REG_XYZ2,
                mx_gs_xyz(mx_gs_px(v & 1 ? 640 : 0), mx_gs_py(v >> 1 ? hy : 0), 0));
  }
  c = mx_e_ad(c, MX_GS_REG_TEST_1, mx_gs_test(0, 0, 0, 0, 0, 0, 1, 2));
  mx_frame_commit(c);
}

/* The state for terrain, water or trees. */
static void gs_state(u32 tbp, u32 tw, u32 th, u32 tbw, int blend, int atest) {
  u64 *c = mx_frame_direct(9);
  c = gs_regs(c, 8);
  c = mx_e_ad(c, MX_GS_REG_TEX0_1, mx_gs_tex0(tbp, tbw, MX_GS_PSM_CT32, tw, th, 1, 0));
  c = mx_e_ad(c, MX_GS_REG_TEX1_1, mx_gs_tex1(0, 0, 1, 1, 0, 0, 0));
  c = mx_e_ad(c, MX_GS_REG_CLAMP_1, mx_gs_clamp(0, 0, 0, 0, 0, 0));
  c = mx_e_ad(c, MX_GS_REG_FOGCOL, HORIZON & 0xFFFFFF);
  c = mx_e_ad(c, MX_GS_REG_ALPHA_1, mx_gs_alpha(0, 1, 0, 1, 0));
  c = mx_e_ad(c, MX_GS_REG_TEST_1,
              mx_gs_test(atest, 6 /* GREATER */, 0x40, 0, 0, 0, 1, 2));
  c = mx_e_ad(c, MX_GS_REG_RGBAQ, mx_gs_rgbaq(0x80, 0x80, 0x80, 0x80, 1.0f));
  c = mx_e_ad(c, MX_GS_REG_PABE, 0);
  mx_frame_commit(c);
  (void)blend;
}

static float fog_near, fog_far;
static void set_fog(float *f) {
  f[0] = -255 / (fog_far - fog_near), f[1] = 255 * fog_far / (fog_far - fog_near), f[2] = f[3] = 0;
}
static const float SUN[3] = {0.48f, 0.74f, 0.47f};
static void set_light(float *l, float *lc, float *amb) {
  for (int k = 0; k < 3; k++)
    l[k] = SUN[k] / 127, lc[k] = 0.85f, amb[k] = 0.32f;
  l[3] = lc[3] = 0, amb[3] = 1;
}

struct draw {
  int c, level, clip;
  float dist;
};
static struct draw dl[NCHUNK];
static u32 frame_vertices;

static void draw_terrain(int c, int level) {
  float m[4][4];
  chunk_mvp(m, itof(ch[c].cx) * CHUNK, itof(ch[c].cz) * CHUNK);
  for (int i = 0; i < 4; i++)
    for (int k = 0; k < 4; k++)
      uni[i][k] = m[i][k];
  const struct mx_shader *s;
  const void *st[4];
  u32 n;
  int tex = 0;
  if (level == 3) {
    s = &sh_l3;
    /* lm[k] = component k of three lights: the sun, a sky fill, a bounce */
    static const float L[3][3] = {{0.48f, 0.74f, 0.47f}, {-0.3f, 0.9f, -0.3f}, {0, -1, 0}};
    static const float C[3][3] = {{0.8f, 0.76f, 0.66f}, {0.12f, 0.16f, 0.24f}, {0.08f, 0.07f, 0.05f}};
    for (int j = 0; j < 3; j++)
      for (int k = 0; k < 3; k++)
        uni[4 + k][j] = L[j][k] / 127, uni[7 + j][k] = C[j][k];
    for (int k = 0; k < 3; k++)
      uni[4 + k][3] = uni[7 + k][3] = 0;
    uni[4][3] = 1.2f; /* the nearest w */
    uni[10][0] = uni[10][1] = uni[10][2] = 0.22f, uni[10][3] = 1;
    set_fog(uni[11]);
    uni[11][2] = 2048, uni[11][3] = 1950; /* centre, guard band */
    st[0] = fpos[c], st[1] = fnrm[c], st[2] = fcol[c], st[3] = fuv[c];
    n = NV_FULL;
    tex = 1;
  } else if (level == 2) {
    s = &sh_tf2;
    set_fog(uni[4]);
    set_light(uni[5], uni[6], uni[7]);
    st[0] = fpos[c], st[1] = fnrm[c], st[2] = fcol[c], st[3] = fuv[c];
    n = NV_FULL;
    tex = 1;
  } else if (level == 1) {
    s = &sh_tf1;
    set_fog(uni[4]);
    set_light(uni[5], uni[6], uni[7]);
    st[0] = fpos[c], st[1] = fnrm[c], st[2] = fcol[c];
    n = NV_FULL;
  } else {
    s = &sh_tf0;
    set_fog(uni[4]);
    int full = level == 0;
    st[0] = full ? (const void *)fpos[c] : (const void *)hpos[c];
    st[1] = full ? (const void *)fcol[c] : (const void *)hcol[c];
    n = full ? NV_FULL : NV_HALF;
  }
  mx_dcache_writeback(uni, sizeof uni);
  bind(s);
  mx_frame_shader_draw(s, (u32)mx_gs_prim(MX_GS_PRIM_TRISTRIP, 1, tex, 1, 0, 0, 0, 0, 0), uni, st,
                       n);
  frame_vertices += n;
}

/* The crack filler of edge e of half resolution chunk c (bound: tf0). */
static void draw_edge(int c, int e) {
  float m[4][4];
  chunk_mvp(m, itof(ch[c].cx) * CHUNK, itof(ch[c].cz) * CHUNK);
  for (int i = 0; i < 4; i++)
    for (int k = 0; k < 4; k++)
      uni[i][k] = m[i][k];
  set_fog(uni[4]);
  mx_dcache_writeback(uni, sizeof uni);
  const void *st[2] = {epos[c][e], ecol[c][e]};
  mx_frame_shader_draw(&sh_tf0, (u32)mx_gs_prim(MX_GS_PRIM_TRISTRIP, 1, 0, 1, 0, 0, 0, 0, 0), uni,
                       st, NV_EDGE);
  frame_vertices += NV_EDGE;
}

/* A water grid of g x g points over a chunk, as one strip. */
static void make_water_grid(float (*w)[3], int g) {
  u32 n = 0;
  for (int r = 0; r < g - 1; r++)
    for (int x = 0; x < g; x++)
      for (int d = 0; d < 2; d++) {
        int reps = (r && !x && !d) || (r < g - 2 && x == g - 1 && d) ? 2 : 1;
        for (int t = 0; t < reps; t++, n++) {
          w[n][0] = itof(x) * (CHUNK / itof(g - 1)), w[n][1] = WATER_LEVEL;
          w[n][2] = itof(r + d) * (CHUNK / itof(g - 1));
        }
      }
  mx_dcache_writeback(w, n * sizeof w[0]);
}

/* The clipping uniform of the shaders: centre, guard band, nearest w. */
static void set_clip(float *f) { f[0] = 2048, f[1] = 1950, f[2] = 1.2f, f[3] = 0; }

/* The dust, in the box laid around the eye (bound: dustdraw). */
static void draw_dust(void) {
  float o[3] = {cam.x / DUST_BOX - 0.5f, cam.y / DUST_BOX - 0.5f, cam.z / DUST_BOX - 0.5f};
  float m[4][4];
  chunk_mvp(m, o[0] * DUST_BOX, o[2] * DUST_BOX);
  for (int i = 0; i < 4; i++)
    for (int k = 0; k < 4; k++)
      uni[i][k] = m[i][k];
  set_fog(uni[4]);
  uni[5][0] = o[0], uni[5][1] = o[1], uni[5][2] = o[2], uni[5][3] = DUST_BOX;
  uni[6][0] = o[1] * DUST_BOX, uni[6][1] = 0.035f * FX, uni[6][2] = 0.035f * FY, uni[6][3] = 0;
  uni[7][0] = 255, uni[7][1] = 238, uni[7][2] = 200, uni[7][3] = 96;
  set_clip(uni[8]);
  mx_dcache_writeback(uni, sizeof uni);
  const void *st[1] = {dust};
  mx_frame_shader_draw(&sh_dust, (u32)mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 0, 1, 1, 0, 0, 0, 0), uni,
                       st, NDUST);
  frame_vertices += NDUST * 2;
}

static void draw_water(int c, float t, int fine) {
  float m[4][4];
  chunk_mvp(m, itof(ch[c].cx) * CHUNK, itof(ch[c].cz) * CHUNK);
  for (int i = 0; i < 4; i++)
    for (int k = 0; k < 4; k++)
      uni[i][k] = m[i][k];
  set_fog(uni[4]);
  uni[5][0] = t * 1.7f, uni[5][1] = 0.6f;
  uni[5][2] = itof(ch[c].cx) * CHUNK, uni[5][3] = itof(ch[c].cz) * CHUNK;
  uni[6][0] = 40, uni[6][1] = 90, uni[6][2] = 120, uni[6][3] = 70;
  set_clip(uni[7]);
  mx_dcache_writeback(uni, sizeof uni);
  const void *st[1] = {fine ? wfine : wpos};
  u32 n = fine ? NV_GRID(WGF) : NV_GRID(WG);
  mx_frame_shader_draw(&sh_water, (u32)mx_gs_prim(MX_GS_PRIM_TRISTRIP, 1, 0, 1, 1, 0, 0, 0, 0),
                       uni, st, n);
  frame_vertices += n;
}

static void draw_trees(int c) {
  if (!ch[c].ntrees)
    return;
  float m[4][4];
  chunk_mvp(m, itof(ch[c].cx) * CHUNK, itof(ch[c].cz) * CHUNK);
  for (int i = 0; i < 4; i++)
    for (int k = 0; k < 4; k++)
      uni[i][k] = m[i][k];
  set_fog(uni[4]);
  uni[5][0] = 2.6f * FX, uni[5][1] = 9.0f * FY, uni[5][2] = uni[5][3] = 0;
  uni[6][0] = 0, uni[6][1] = 0, uni[6][2] = 32, uni[6][3] = 64;
  set_clip(uni[7]);
  mx_dcache_writeback(uni, sizeof uni);
  const void *st[1] = {trees[c]};
  mx_frame_shader_draw(&sh_tree, (u32)mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 1, 1, 0, 0, 1, 0, 0), uni,
                       st, (u32)ch[c].ntrees);
  frame_vertices += (u32)ch[c].ntrees * 2;
}

/* Text at (x, y) pixels, PATH2 sprites from the font texture. */
static void text(u32 x, u32 y, const char *s) {
  u32 n = 0;
  for (const char *p = s; *p; p++)
    n++;
  u64 *c = mx_frame_direct(6 + 4 * n);
  c = gs_regs(c, 4);
  c = mx_e_ad(c, MX_GS_REG_TEX0_1, mx_gs_tex0(tbp_font, 4, MX_GS_PSM_CT32, 8, 3, 1, 0));
  c = mx_e_ad(c, MX_GS_REG_TEX1_1, mx_gs_tex1(0, 0, 0, 0, 0, 0, 0));
  c = mx_e_ad(c, MX_GS_REG_TEST_1, mx_gs_test(1, 6, 0x40, 0, 0, 0, 1, 1));
  c = mx_e_ad(c, MX_GS_REG_RGBAQ, mx_gs_rgbaq(0x80, 0x80, 0x80, 0x80, 1.0f));
  c = mx_e_giftag(c, n, 1, (s32)mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 1, 0, 0, 0, 1, 0, 0),
                  MX_GIF_FLG_PACKED, 4,
                  mx_gif_regs4(MX_GIF_REG_UV, MX_GIF_REG_XYZ2, MX_GIF_REG_UV, MX_GIF_REG_XYZ2));
  for (const char *p = s; *p; p++, x += 6) {
    u32 g = 0;
    for (u32 i = 0; glyph_chars[i]; i++)
      if (glyph_chars[i] == *p)
        g = i;
    /* texel centres: + 8 (half a texel), or the GS of the PS2 drops the last column
       and row of a 1:1 sprite (PCSX2 does not) */
    c = mx_e_uv(c, g * 6 * 16 + 8, 8);
    /* in lines of a 224-line screen */
    c = mx_e_xyz2(c, mx_gs_px(x), mx_gs_py(y * SH / 224), 0xFFFFFF, 0);
    c = mx_e_uv(c, (g * 6 + 5) * 16 + 8, 7 * 16 + 8);
    c = mx_e_xyz2(c, mx_gs_px(x + 5), mx_gs_py((y + 7) * SH / 224), 0xFFFFFF, 0);
  }
  mx_frame_commit(c);
}

static void fmt_u(char *d, u32 v) {
  char t[12];
  int n = 0;
  do
    t[n++] = (char)('0' + v % 10);
  while (v /= 10);
  while (n)
    *d++ = t[--n];
  *d = 0;
}
static char *cat(char *d, const char *s) {
  while (*s)
    *d++ = *s++;
  *d = 0;
  return d;
}
static char *cat_u(char *d, u32 v) {
  fmt_u(d, v);
  while (*d)
    d++;
  return d;
}

/* ---- the frame ------------------------------------------------------------- */

#define FIELD_CYCLES 4920116u /* EE cycles per field (59.94 Hz) */
enum { LOOKS, MAXCOUNT };
static int preset = LOOKS;
enum { Q_AUTO, Q_L0, Q_L1, Q_L2, Q_L3, Q_HALF, Q_N };
static int quality = Q_AUTO;
static const char *const quality_names[Q_N] = {"AUTO", "L0", "L1", "L2", "L3", "HALF"};
static float radius = 6; /* view distance in chunks */
static u32 frame_no;
static char hud[3][64];

/* The chunks around the camera: ready ones drawn, missing ones queued. */
/* Whether chunk (cx, cz) is in view or about to be: near the eye, or a
   corner within 50 degrees of the horizontal view direction (the view is 70
   degrees wide; the rest is for turning). Only these are loaded, so the
   chunks cover a view distance about twice that of a full circle. */
static int in_view(int cx, int cz) {
  float fx = cam.f[0], fz = cam.f[2], fl = fx * fx + fz * fz;
  if (fl < 1e-6f)
    return 1;
  for (int k = 0; k < 4; k++) {
    float px = itof(cx + (k & 1)) * CHUNK - cam.x, pz = itof(cz + (k >> 1)) * CHUNK - cam.z;
    float d2 = px * px + pz * pz, dot = px * fx + pz * fz;
    if (d2 < 4 * CHUNK * CHUNK || (dot > 0 && dot * dot > 0.413f * d2 * fl))
      return 1;
  }
  return 0;
}

static int queue_pending(int ccx, int ccz, int r) {
  int best = -1;
  float bestd = 1e9f;
  for (int dz = -r; dz <= r; dz++)
    for (int dx = -r; dx <= r; dx++) {
      float d = sqrtf_(itof(dx * dx + dz * dz));
      if (d > itof(r) + 0.5f || !in_view(ccx + dx, ccz + dz))
        continue;
      int cx = ccx + dx, cz = ccz + dz, c = find_chunk(cx, cz);
      if (c >= 0) {
        ch[c].used = frame_no;
        continue;
      }
      if (d < bestd)
        bestd = d, best = (cz + 0x4000) << 15 | (cx + 0x4000); /* >= 0 */
    }
  return best;
}

static void ensure_chunks(void) {
  int ccx = (int)(s32)floorf_(cam.x / CHUNK), ccz = (int)(s32)floorf_(cam.z / CHUNK);
  int r = (int)(s32)radius + 1;
  if (gen.c >= 0 || gen.build_c >= 0)
    return;
  int want = queue_pending(ccx, ccz, r);
  if (want < 0)
    return;
  /* the chunk unused the longest */
  int c = -1;
  u32 oldest = 0xFFFFFFFFu;
  for (int i = 0; i < NCHUNK; i++)
    if (ch[i].state == C_FREE) {
      c = i;
      break;
    } else if (ch[i].state == C_READY && ch[i].used < oldest && ch[i].used + 2 < frame_no) {
      oldest = ch[i].used;
      c = i;
    }
  if (c < 0)
    return;
  ch[c].cx = (want & 0x7FFF) - 0x4000, ch[c].cz = (want >> 15) - 0x4000;
  ch[c].used = frame_no;
  gen_start(c);
}

static void frame(float t) {
  camera_update(t);
  ensure_chunks();
  gen_poll();
  /* fog towards the end of the view */
  fog_far = (radius - 0.3f) * CHUNK;
  fog_near = fog_far * 0.45f;
  int ccx = (int)(s32)floorf_(cam.x / CHUNK), ccz = (int)(s32)floorf_(cam.z / CHUNK);
  int r = (int)(s32)radius;
  u32 nd = 0;
  for (int c = 0; c < NCHUNK; c++) {
    if (ch[c].state != C_READY)
      continue;
    int dx = ch[c].cx - ccx, dz = ch[c].cz - ccz;
    if (dx * dx + dz * dz > (r + 1) * (r + 1))
      continue;
    float mx = itof(ch[c].cx) * CHUNK + CHUNK / 2 - cam.x;
    float mz = itof(ch[c].cz) * CHUNK + CHUNK / 2 - cam.z;
    float d = sqrtf_(mx * mx + mz * mz) / CHUNK;
    if (d > radius + 0.7f)
      continue;
    int v = chunk_visible(&ch[c]);
    if (!v)
      continue;
    int level;
    if (preset == MAXCOUNT)
      level = 0;
    else if (quality == Q_AUTO)
      level = d < 1.3f ? 3 : d < 2.4f ? 2 : d < 3.8f ? 1 : -1; /* -1: half res */
    else
      level = quality == Q_HALF ? -1 : quality - Q_L0;
    if (v == 2)
      level = 3; /* needs clipping */
#ifdef PROCGEN_NOCLIP
    if (v == 2)
      continue;
#endif
    dl[nd++] = (struct draw){c, level, v == 2, d};
  }
  /* the level drawn at each chunk around the camera, for the crack fillers */
#define LG (2 * MAX_R + 5)
  static s8 lvl[LG][LG];
  for (int z = 0; z < LG; z++)
    for (int x = 0; x < LG; x++)
      lvl[z][x] = -2;
  for (u32 i = 0; i < nd; i++) {
    int x = ch[dl[i].c].cx - ccx + LG / 2, z = ch[dl[i].c].cz - ccz + LG / 2;
    if (x >= 0 && x < LG && z >= 0 && z < LG)
      lvl[z][x] = (s8)dl[i].level;
  }
#ifdef PROCGEN_DEBUG
  if (frame_no % 60 == 0)
    mx_printf("t %d cam y %d terrain %d\n", (s32)t, (s32)cam.y,
              (s32)terrain_height(cam.x, cam.z));
  if (frame_no == 120) {
    mx_printf("cam %d %d %d (x10) h %d f %d %d %d u %d %d %d\n", (s32)(cam.x * 10),
              (s32)(cam.y * 10), (s32)(cam.z * 10), (s32)terrain_height(cam.x, cam.z),
              (s32)(cam.f[0] * 100), (s32)(cam.f[1] * 100), (s32)(cam.f[2] * 100),
              (s32)(cam.u[0] * 100), (s32)(cam.u[1] * 100), (s32)(cam.u[2] * 100));
    for (u32 i = 0; i < nd; i++) {
      struct chunk *k = &ch[dl[i].c];
      if (dl[i].dist > 2.5f)
        continue;
      float m[4][4];
      chunk_mvp(m, itof(k->cx) * CHUNK, itof(k->cz) * CHUNK);
      float *v = fpos[dl[i].c][0], P[4];
      for (int q = 0; q < 4; q++)
        P[q] = m[0][q] * v[0] + m[1][q] * v[1] + m[2][q] * v[2] + m[3][q];
      mx_printf("chunk %d,%d d %d/10 lvl %d clip %d h %d..%d v0 (%d %d %d) -> X %d Y %d w %d\n",
                k->cx, k->cz, (s32)(dl[i].dist * 10), dl[i].level, dl[i].clip, (s32)k->hmin,
                (s32)k->hmax, (s32)v[0], (s32)v[1], (s32)v[2], (s32)(P[0] / P[3]),
                (s32)(P[1] / P[3]), (s32)P[3]);
    }
  }
#endif
  mx_frame_begin(HORIZON & 0xFF, HORIZON >> 8 & 0xFF, HORIZON >> 16 & 0xFF);
  draw_sky();
  frame_vertices = 0;
  /* by shader, so each is bound once */
  static const int order[5] = {-1, 0, 1, 2, 3};
  for (int o = 0; o < 5; o++) {
    int any = 0;
    for (u32 i = 0; i < nd; i++)
      if (dl[i].level == order[o]) {
        if (!any && order[o] >= 2)
          gs_state(tbp_detail, 6, 6, 1, 0, 0);
        any = 1;
        draw_terrain(dl[i].c, dl[i].level);
        if (order[o] < 0) {
          static const int ex[4] = {0, 0, -1, 1}, ez[4] = {-1, 1, 0, 0};
          int x = ch[dl[i].c].cx - ccx + LG / 2, z = ch[dl[i].c].cz - ccz + LG / 2;
          for (int e = 0; e < 4; e++) {
            int nx = x + ex[e], nz = z + ez[e];
            if (nx >= 0 && nx < LG && nz >= 0 && nz < LG && lvl[nz][nx] >= 0)
              draw_edge(dl[i].c, e);
          }
        }
        if ((i & 7) == 0)
          gen_poll();
      }
  }
  if (preset == LOOKS) {
    gs_state(tbp_tree, 5, 6, 1, 1, 1);
    bind(&sh_water);
    for (u32 i = 0; i < nd; i++)
      if (ch[dl[i].c].water && dl[i].dist < radius - 0.5f)
        draw_water(dl[i].c, t, dl[i].clip);
    bind(&sh_tree);
    for (u32 i = 0; i < nd; i++)
      if (dl[i].dist < 3.2f)
        draw_trees(dl[i].c);
    bind(&sh_dust);
    draw_dust();
  }
  for (int l = 0; l < 3; l++)
    text(8, 6 + 9 * (u32)l, hud[l]);
  mx_frame_end();
  /* after the frame is queued, while VU1 and the GS draw it: the dust on VU0
     between two batches of heights (the one in flight first), then the
     streams of a chunk */
  if (preset == LOOKS) {
    if (gen.c >= 0 && !gen.kick)
      mx_vuq_wait(&q0, gen.e);
    gen.hold = 1;
    gen_poll();
    gen.hold = 0;
    dust_pending = 1;
    dust_update();
    gen_kick();
  } else {
    gen_poll();
  }
  gen_build();
  frame_no++;
}

/* ---- main -------------------------------------------------------------------- */

static u8 ring[64 * 1024] MX_ALIGNED(64 * 1024);

static u32 ee_waited; /* mx_frame_stats waits at the last report */

/* The busy shares on the HUD (line 3) once a second: from the change of the
   sampler's counts since the last time (they start again at 0 when the
   sampling is restarted: hud_busy_restart). */
static u32 hb_last[MX_TS_N], hb_last_n;
static void hud_busy_restart(void) {
  for (int u = 0; u < MX_TS_N; u++)
    hb_last[u] = 0;
  hb_last_n = 0;
}
static char *hud_busy(char *p) {
  u32 now[MX_TS_N], n = mx_trace_busy(now), dn = n - hb_last_n;
  static const int shown[3] = {6, 8, 5};
  static const char *const names[3] = {"VU1 ", "% GS ", "% VU0 "};
  for (int i = 0; i < 3; i++) {
    p = cat(p, names[i]);
    p = cat_u(p, dn ? (now[shown[i]] - hb_last[shown[i]]) * 100 / dn : 0);
  }
  for (int u = 0; u < MX_TS_N; u++)
    hb_last[u] = now[u];
  hb_last_n = n;
  return cat(p, "%");
}
static void report(const char *name, u32 frames, u32 cycles, u32 vs, u32 *busy, u32 ns,
                   u32 verts) {
  static const char *units[MX_TS_N] = {"VIF0", "VIF1", "GIFDMA", "fromSPR", "toSPR",
                                       "VU0",  "VU1",  "GIF",    "GS", "VU1-GIF-wait"};
  mx_printf("procgen %s: %u frames in %u vsyncs, missed %u, %u vertices/frame, "
            "%u.%u fps; busy",
            name, frames, vs, mx_gs.missed, verts, MX_EE_HZ / (cycles / frames),
            MX_EE_HZ * 10 / (cycles / frames) % 10);
  for (int u = 5; u < MX_TS_N; u++)
    mx_printf(" %s %u%%", units[u], ns ? busy[u] * 100 / ns : 0);
  /* the EE: busy but for its waits in the frame pipeline (for a back buffer,
     for ring space) */
  u32 w = mx_frame_stats.wait_buffer + mx_frame_stats.wait_ring, dw = w - ee_waited;
  ee_waited = w;
  mx_printf(" EE %u%%", dw < cycles ? (u32)((u64)(cycles - dw) * 100 / cycles) : 0);
  mx_printf("; view %u.%u chunks; slack %u%%; build_chunk max %u us\n", (u32)(s32)radius,
            (u32)(s32)(radius * 10) % 10, mx_gs.slack / (FIELD_CYCLES / 100),
            gen.build_max / (MX_EE_HZ / 1000000));
  gen.build_max = 0;
}

#ifdef PROCGEN_DEBUG
#define STEP(s) mx_printf("procgen step: %s\n", s)
#else
#define STEP(s) ((void)0)
#endif

/* The view distance, from how early the frames are ready (mx_gs.slack, the
   time from a frame's FINISH to its vblank): 0.25 chunks longer after 90
   frames that were all ready 20 % of a field early, 0.25 shorter as soon as
   one is ready less than 8 % early (or late), then 3 frames to take effect. */
static void budget(u32 missed_before) {
  static u32 calm, hold;
  if (hold) {
    hold--;
    return;
  }
  u32 sl = mx_gs.slack;
  if (mx_gs.missed != missed_before || sl < FIELD_CYCLES / 100 * 8) {
    radius = radius > 3.5f ? radius - 0.25f : radius;
    calm = 0;
    hold = 3;
  } else if (sl > FIELD_CYCLES / 100 * 20) {
    if (++calm > 90 && radius < MAX_R - 1) {
      radius += 0.25f;
      calm = 0;
    }
  } else {
    calm = 0;
  }
}

/* The buttons pressed on pad 0. With PROCGEN_PADSIM, a script instead (a
   frame number and the buttons), which runs every control of the
   interactive mode on its own. */
#ifdef PROCGEN_PADSIM
static u32 pad_frame;
static u32 pad_pressed(void) {
  static const u32 script[][2] = {
      {60, MX_PAD_START},                                  /* interactive */
      {240, MX_PAD_SELECT},  {420, MX_PAD_SELECT},          /* L0, L1 */
      {600, MX_PAD_SELECT},  {780, MX_PAD_SELECT},          /* L2, L3 */
      {960, MX_PAD_SELECT},  {1140, MX_PAD_SELECT},         /* HALF, AUTO */
      {1320, MX_PAD_START},  {1500, MX_PAD_START},          /* no vsync, vsync */
      {1680, MX_PAD_R1},     {1860, MX_PAD_L1 },            /* view fixed */
      {2040, MX_PAD_SQUARE}, {2220, MX_PAD_TRIANGLE},       /* budget, end */
  };
  u32 f = pad_frame++, b = 0;
  for (u32 i = 0; i < sizeof script / sizeof script[0]; i++)
    if (script[i][0] == f)
      b = script[i][1];
  if (b)
    mx_printf("procgen: pad %04x at frame %u\n", b, f);
  return b;
}
#else
static u32 pad_pressed(void) { return mx_pad_pressed(0); }
#endif

/* The interactive mode (see the top): until TRIANGLE. The time is the
   vblank count, so the flight keeps its speed without vsync. */
static void interactive(float t0) {
  int vsync = 1, fixed = 0;
  u32 v0 = mx_gs.vsync, n = 0, vsum = 0, busy[MX_TS_N];
  mx_trace_sample(1000);
  hud_busy_restart();
  u32 tp = mx_count(), fp = mx_gs.frames;
  for (;;) {
    u32 b = pad_pressed();
    if (b & MX_PAD_TRIANGLE)
      break;
    if (b & MX_PAD_SELECT)
      quality = (quality + 1) % Q_N;
    if (b & MX_PAD_START)
      mx_frame_vsync(vsync = !vsync);
    if (b & (MX_PAD_L1 | MX_PAD_R1)) {
      radius += b & MX_PAD_R1 ? 0.5f : -0.5f;
      radius = clampf(radius, 2, MAX_R - 1);
      fixed = 1;
    }
    if (b & MX_PAD_SQUARE)
      fixed = 0;
    u32 before = mx_gs.missed;
    frame(t0 + itof((s32)(mx_gs.vsync - v0)) / 60);
    vsum += frame_vertices;
    n++;
    if (!fixed && vsync)
      budget(before);
    char *p = hud[0];
    p = cat(p, "PROCGEN  Q ");
    p = cat(p, quality_names[quality]);
    p = cat(p, vsync ? "  VSYNC  VIEW " : "  NO VSYNC  VIEW ");
    p = cat_u(p, (u32)(s32)(radius * 10));
    cat(p, fixed ? "/10 FIXED" : "/10 BUDGET");
    p = hud[1];
    p = cat_u(p, frame_vertices);
    cat(p, " VERTICES");
    if (n == 60) {
      u32 ns = mx_trace_busy(busy), cyc = mx_count() - tp, fr = mx_gs.frames - fp;
      (void)ns;
      p = hud[2];
      p = cat_u(p, (u32)((u64)fr * MX_EE_HZ / cyc));
      p = cat(p, " FPS  ");
      p = cat_u(p, (u32)((u64)vsum * MX_EE_HZ / cyc / 1000));
      p = cat(p, "K VERTICES/S  ");
      hud_busy(p);
#ifdef PROCGEN_PADSIM
      mx_printf("procgen: %s | %s | %s\n", hud[0], hud[1], hud[2]);
#endif
      mx_trace_sample(0);
      mx_trace_sample(1000);
      hud_busy_restart();
      tp = mx_count(), fp = mx_gs.frames, n = 0, vsum = 0;
    }
  }
  mx_trace_sample(0);
  mx_frame_vsync(1);
  mx_frame_stop();
}

int main(void) {
  STEP("start");
  mx_gs_init(SW, SH);
  mx_vu_reset(3);
  fbm_k = MX_VUK_KERNEL(FBM, 0, fbm_vu, fbm_vu_end);
  mx_vu_load(0, fbm_k.code, fbm_k.code_bytes, 0);
  mx_vuq_init(&q0, 0, 1);
  dust_init();
  mx_vuq_readback_spr(&q0, (void *)0x70000000, 0x2000);
  fbm_hdr[4 * VUK_FBM_OCT] = OCT;
  gen.per = mx_kernel_max_items(&fbm_k);
  sh_tf0 = MX_VUK_SHADER(TF0, tf0_vu, tf0_vu_end);
  sh_tf1 = MX_VUK_SHADER(TF1, tf1_vu, tf1_vu_end);
  sh_tf2 = MX_VUK_SHADER(TF2, tf2_vu, tf2_vu_end);
  sh_l3 = MX_VUK_SHADER(LVL3, lvl3_vu, lvl3_vu_end);
  sh_water = MX_VUK_SHADER(WATER, water_vu, water_vu_end);
  sh_tree = MX_VUK_SHADER(TREE, tree_vu, tree_vu_end);
  sh_dust = MX_VUK_SHADER(DUSTDRAW, dustdraw_vu, dustdraw_vu_end);
  STEP("vu0 queue");
  mx_frame_init(ring, sizeof ring, (void *)0x70002000, 0x2000);
  STEP("frame");
  make_textures();
  if (mx_pad_init() < 0)
    mx_printf("procgen: no pad driver\n");
  make_water_grid(wpos, WG);
  make_water_grid(wfine, WGF);
  STEP("textures");
  cat(hud[0], "PROCGEN");

  /* loading: the chunks of the first view */
  camera_update(0);
  u32 loaded = 0, t0 = mx_count();
  for (int i = 0; i < 400; i++) {
#ifdef PROCGEN_DEBUG
    if (i < 2)
      mx_printf("load %d: pending %d sqrt(2) %d/1000 radius %d/10 cam %d %d\n", i,
                queue_pending((int)(s32)floorf_(cam.x / CHUNK), (int)(s32)floorf_(cam.z / CHUNK),
                              (int)(s32)radius + 1),
                (s32)(sqrtf_(2) * 1000), (s32)(radius * 10), (s32)cam.x, (s32)cam.z);
#endif
    ensure_chunks();
    if (i < 3)
      STEP("chunk queued");
#ifdef PROCGEN_DEBUG
    u32 w0 = mx_count();
#endif
    while (gen.c >= 0) {
      gen_poll();
#ifdef PROCGEN_DEBUG
      if (mx_count() - w0 > MX_EE_HZ) {
        mx_printf("procgen: chunk stuck at %u: VIF0_STAT %08x VIF0 CHCR %08x MADR %08x QWC %u "
                  "TADR %08x VPU_STAT %08x done %d\n",
                  gen.at, MX_R32(MX_VIF_VIF0_STAT), MX_R32(0x10008000), MX_R32(0x10008010),
                  MX_R32(0x10008020), MX_R32(0x10008030), mx_vpu_stat(), mx_vuq_done(&q0, gen.e));
        w0 = mx_count();
      }
#endif
    }
    loaded += (u32)gen_build();
    if (i < 3)
      STEP("chunk done");
    if (queue_pending((int)(s32)floorf_(cam.x / CHUNK), (int)(s32)floorf_(cam.z / CHUNK),
                      (int)(s32)radius + 1) < 0)
      break;
  }
  cam.y = 0; /* placed on the loaded terrain */
  camera_update(0);
  mx_printf("procgen: %u chunks loaded in %u ms, %u cycles per chunk (VU0 heights + EE "
            "streams)\n",
            loaded, (mx_count() - t0) / (MX_EE_HZ / 1000), gen.cycles);

  /* "looks" at one frame per vsync */
  u32 busy[MX_TS_N];
  u32 frames = PROCGEN_SECONDS * 60, period = 600;
  u32 v0 = mx_gs.vsync, missed0, vsum = 0, first_miss = ~0u;
  mx_gs.missed = 0;
  mx_frame_stats.wait_buffer = mx_frame_stats.wait_ring = ee_waited = 0;
  mx_trace_sample(1000);
  hud_busy_restart();
  u32 tp = mx_count(), vp = mx_gs.vsync;
  for (u32 f = 0; f < frames; f++) {
    if (pad_pressed() & MX_PAD_START) {
      mx_trace_sample(0);
      interactive(itof((s32)f) / 60);
      mx_printf("procgen: interactive mode ended\n");
      MX_TEST_END(0);
      return 0;
    }
    u32 before = mx_gs.missed;
    frame(itof((s32)f) / 60);
    vsum += frame_vertices;
    /* the view distance: shorter after a missed vblank, longer after calm */
    if (mx_gs.missed != before && first_miss == ~0u)
      first_miss = f;
    budget(before);
    char *p = hud[1];
    p = cat(p, "LOOKS ");
    p = cat_u(p, frame_vertices);
    p = cat(p, " VERTICES  VIEW ");
    p = cat_u(p, (u32)(s32)(radius * 10));
    cat(p, "/10 CHUNKS");
    if (f % 60 == 59) {
      p = hud_busy(hud[2]);
      p = cat(p, " MISSED ");
      cat_u(p, mx_gs.missed);
    }
    if (f % period == period - 1) {
      u32 ns = mx_trace_busy(busy);
      report("looks", period, mx_count() - tp, mx_gs.vsync - vp, busy, ns, vsum / period);
      mx_trace_sample(0);
      mx_trace_sample(1000);
      hud_busy_restart();
      tp = mx_count(), vp = mx_gs.vsync, vsum = 0;
    }
  }
  missed0 = mx_gs.missed;
  mx_trace_sample(0);
  u32 vs_looks = mx_gs.vsync - v0;
  /* show the last "looks" frame, save it */
  u32 last = mx_gs.frames - 1;
  while (mx_gs.shown < last + 1)
    ;
  mx_frame_stop();
  mx_gs_save_tga((int)(last & 1), "host:procgen_looks.tga");
  mx_frame_init(ring, sizeof ring, (void *)0x70002000, 0x2000);
  bound = 0;
  mx_vu_forget(1);

  /* "max count": level 0 at full resolution, the largest view, no vsync */
  preset = MAXCOUNT;
  radius = MAX_R;
  for (int i = 0; i < 400; i++) { /* load the larger view */
    ensure_chunks();
    while (gen.c >= 0)
      gen_poll();
    gen_build();
    if (queue_pending((int)(s32)floorf_(cam.x / CHUNK), (int)(s32)floorf_(cam.z / CHUNK),
                      MAX_R + 1) < 0)
      break;
  }
  mx_frame_vsync(0);
  u32 mframes = 600;
  float tbase = itof((s32)frames) / 60;
  mx_trace_sample(1000);
  hud_busy_restart();
  tp = mx_count(), vp = mx_gs.vsync, vsum = 0;
  ee_waited = mx_frame_stats.wait_buffer + mx_frame_stats.wait_ring;
  u32 base = mx_gs.frames;
  for (u32 f = 0; f < mframes; f++) {
    frame(tbase);
    vsum += frame_vertices;
    char *p = hud[1];
    p = cat(p, "MAX COUNT ");
    p = cat_u(p, frame_vertices);
    cat(p, " VERTICES");
    if (f % 60 == 59)
      hud_busy(hud[2]);
  }
  u32 d0 = mx_count();
  while (mx_gs.frames_done < base + mframes && mx_count() - d0 < 300000000u)
    ;
  u32 ns = mx_trace_busy(busy);
  mx_trace_sample(0);
  u32 cyc = mx_count() - tp;
  report("max count", mframes, cyc, mx_gs.vsync - vp, busy, ns, vsum / mframes);
  mx_printf("procgen max count: %u vertices per second\n",
            (u32)((u64)vsum * MX_EE_HZ / cyc));
  mx_frame_vsync(1);
  last = mx_gs.frames - 1;
  d0 = mx_count();
  while (mx_gs.frames_done < last + 1 && mx_count() - d0 < 300000000u)
    ;
  mx_frame_stop();
  mx_gs_save_tga((int)(last & 1), "host:procgen_max.tga");

  mx_printf("procgen looks: %u frames in %u vsyncs, %u missed", frames, vs_looks, missed0);
  if (missed0)
    mx_printf(" (the first at frame %u)", first_miss);
  mx_printf("\n");
  int fail = missed0 != 0;
  MX_TEST_END(fail);
  return fail;
}
