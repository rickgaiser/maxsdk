/* Model demo: a model converted from glTF (tools/model/gltf2mxm.py,
   demos/model/mxm.h), turning, drawn by VU1 with HLSL vertex shaders
   (demos/model/m*.hlsl), each as strips or indexed: the same shader built a
   second time with opt -vu-indexed, which transforms each vertex of a
   cluster once and gathers the strip by its indices (llvm VUEntryABI.md).
   Up to 16 x 16 copies, to see how many vertices and triangles a second each
   shader draws.

     mflat  position, one colour
     mlit   + normal, one light
     mtex   + texture, perspective correct
     mskin  + skinning: four joints a vertex, the animation baked by the
            converter (a skinned model only)

   The model: host:<argv[1]> (an .mxm file, from ps2client's directory), else
   the one built in (Fox, demos/model/assets).

   The run, without a pad: for each shader one frame as strips and one
   indexed, which must be the same picture (the outputs are the same, bit
   for bit); then each shader both ways without vsync for BENCH_FRAMES
   frames, BENCH_COPIES x BENCH_COPIES copies (frames, triangles, vertices
   and indices a second, the units' busy shares); then a screenshot
   (host:model.tga). START on pad 0 during the run: the interactive mode:

     left stick, D-pad   turn the view            right stick, L1/R1  zoom
     SQUARE              next shader               CROSS              strips / indexed
     TRIANGLE            copies 1, 2x2 .. 16x16    CIRCLE             next animation
     L2                  turning on/off            R2                 vsync on/off
     SELECT + START      end

   -DMODEL_RECORD=<frames>: instead, that many frames of the model walking
   (or turning), MODEL_RECORD_COPIES x MODEL_RECORD_COPIES copies (1), 30 a
   second whatever the speed, each saved as host:rec_<n>.tga
   (tools/model/frames2video.py makes a video of them). */

#include <mx/mx.h>
#include <mx/pad.h>
#include <mx/sif.h>
#include "hud.h"
#include "mflat.h"
#include "mflat_ix.h"
#include "mlit.h"
#include "mlit_ix.h"
#include "mskin.h"
#include "mskin_ix.h"
#include "mtex.h"
#include "mtex_ix.h"
#include "mxm.h"

extern const u8 mflat_vu[], mflat_vu_end[], mlit_vu[], mlit_vu_end[], mtex_vu[], mtex_vu_end[],
    mskin_vu[], mskin_vu_end[], mflat_ix_vu[], mflat_ix_vu_end[], mlit_ix_vu[], mlit_ix_vu_end[],
    mtex_ix_vu[], mtex_ix_vu_end[], mskin_ix_vu[], mskin_ix_vu_end[];
extern const u8 model_fox[], model_fox_end[];

#ifndef BENCH_FRAMES
#define BENCH_FRAMES 180
#endif
#ifndef BENCH_COPIES
#define BENCH_COPIES 8
#endif

/* the uniforms are in the same places for the strip and the indexed
   programs (in their context) */
_Static_assert(VU_MFLAT_MVP == VU_IX_MFLAT_MVP && VU_MFLAT_COL == VU_IX_MFLAT_COL, "");
_Static_assert(VU_MLIT_LIGHT == VU_IX_MLIT_LIGHT && VU_MLIT_AMB == VU_IX_MLIT_AMB, "");
_Static_assert(VU_MTEX_LIGHT == VU_IX_MTEX_LIGHT && VU_MTEX_AMB == VU_IX_MTEX_AMB, "");
_Static_assert(VU_MSKIN_JOINT == VU_IX_MSKIN_JOINT && VU_MSKIN_AMB == VU_IX_MSKIN_AMB, "");
#define JOINTS 24 /* demos/model/mskin.hlsl */
_Static_assert(VU_IX_MSKIN_UNI_QW == VU_IX_MSKIN_JOINT + 4 * JOINTS, "");

/* the screen: 640 x 448 interlaced, every frame drawn in full */
#define SW 640
#define SH 448
static const float FX = 457, FY = 457 * 0.467f * SH / 224, ZK = 1000000;
/* the model's radius in world units, and the guard band (pixels from the
   centre that a vertex may reach: GS coordinates are 0 .. 4096) */
#define R 50.0f
#define GUARD 1900.0f

/* ---- small math (no libm) ------------------------------------------------------ */

static float sinf_(float a) {
  while (a > 3.14159265f)
    a -= 6.28318531f;
  while (a < -3.14159265f)
    a += 6.28318531f;
  float a2 = a * a;
  return a * (1 - a2 / 6 * (1 - a2 / 20 * (1 - a2 / 42 * (1 - a2 / 72))));
}
static float cosf_(float a) { return sinf_(a + 1.57079633f); }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float itof(s32 v) { return (float)v; }

/* ---- the model -------------------------------------------------------------------- */

#define MAX_FILE (12u << 20)
#define MAX_CLUSTER 16384
#define MAX_MAT 16
static u8 file_buf[MAX_FILE] MX_ALIGNED(64);
static u8 idx_buf[2][1u << 20] MX_ALIGNED(64); /* the indices times 2 and times 3 */
static struct mx_ibatch ibatch[2][MAX_CLUSTER];

static struct {
  const struct mxm_header *h;
  const u8 *base;
  const struct mxm_cluster *cl;
  const struct mxm_strip *strip;
  const struct mxm_mat *mat;
  const struct mxm_pal *pal;
  const struct mxm_anim *anim;
  u32 tbp[MAX_MAT];
  u32 tbp_free; /* the first block after the textures */
  const char *name;
} M;

static const void *sec(int s) { return M.h->off[s] ? M.base + M.h->off[s] : 0; }

/* The model at base (size bytes): checked, its indices made for each
   number of outputs, its textures uploaded. Returns 0, or -1. */
static int model_open(const u8 *base, u32 size, const struct mx_shader *const *ix, u32 nix) {
  const struct mxm_header *h = (const void *)base;
  M.h = h, M.base = base;
  if (size < sizeof *h || h->magic != MXM_MAGIC || h->bytes > size) {
    mx_printf("model: not an MXM file\n");
    return -1;
  }
  for (int s = 0; s < MXM_NSEC; s++)
    if (h->off[s] >= h->bytes || h->off[s] % 64) {
      mx_printf("model: section %d at %u\n", s, h->off[s]);
      return -1;
    }
  if (h->ncluster > MAX_CLUSTER || h->nmat > MAX_MAT || !h->ncluster || !h->nmat) {
    mx_printf("model: %u clusters, %u materials\n", h->ncluster, h->nmat);
    return -1;
  }
  M.cl = sec(MXM_CLUSTER), M.strip = sec(MXM_STRIP), M.mat = sec(MXM_MAT);
  M.pal = sec(MXM_PAL), M.anim = sec(MXM_ANIM);
  /* the clusters must fit every indexed program */
  for (u32 i = 0; i < nix; i++)
    if (h->max_nv > ix[i]->max_n || h->max_ni > ix[i]->max_i) {
      mx_printf("model: made for %u vertices and %u indices, a program has %u and %u\n",
                h->max_nv, h->max_ni, ix[i]->max_n, ix[i]->max_i);
      return -1;
    }
  const u8 *idx = sec(MXM_IDX);
  u32 idx_bytes = 0;
  for (u32 c = 0; c < h->ncluster; c++) {
    const struct mxm_cluster *k = &M.cl[c];
    if (k->nv > h->max_nv || k->ni > h->max_ni || k->first % 4 || k->first + k->nv > h->nvert ||
        k->idx % 16 || k->mat >= h->nmat || (h->npal && k->pal >= h->npal)) {
      mx_printf("model: cluster %u is not right\n", c);
      return -1;
    }
    u32 end = k->idx + (k->ni + 15u) / 16 * 16;
    idx_bytes = end > idx_bytes ? end : idx_bytes;
  }
  if (idx_bytes > sizeof idx_buf[0]) {
    mx_printf("model: %u bytes of indices\n", idx_bytes);
    return -1;
  }
  for (u32 p = 0; p < h->npal; p++)
    if (M.pal[p].n > JOINTS) {
      mx_printf("model: a palette of %u joints\n", M.pal[p].n);
      return -1;
    }
  /* an index is the vertex times the program's outputs (2 or 3) */
  for (u32 i = 0; i < idx_bytes; i++)
    idx_buf[0][i] = (u8)(idx[i] * 2), idx_buf[1][i] = (u8)(idx[i] * 3);
  mx_dcache_writeback(idx_buf, sizeof idx_buf);
  for (u32 c = 0; c < h->ncluster; c++)
    for (int o = 0; o < 2; o++)
      ibatch[o][c] = (struct mx_ibatch){M.cl[c].first, M.cl[c].nv, M.cl[c].ni,
                                        idx_buf[o] + M.cl[c].idx};
  /* the textures, after the framebuffers */
  u32 tbp = mx_gs.tbp;
  const u8 *tex = sec(MXM_TEX);
  for (u32 m = 0; m < h->nmat; m++) {
    u32 w = 1u << M.mat[m].tw, hh = 1u << M.mat[m].th;
    M.tbp[m] = tbp;
    mx_frame_texture(0, tbp, w < 64 ? 1 : w / 64, MX_GS_PSM_CT32, w, hh, tex + M.mat[m].tex);
    mx_frame_wait_path3();
    tbp += (w * hh * 4 + 8191) / 8192 * 32; /* whole pages */
  }
  M.tbp_free = tbp;
  return 0;
}

/* host:path into file_buf. Returns the size, or -1. */
static int model_read(const char *path) {
  int fd = mx_fio_open(path, MX_FIO_RDONLY);
  if (fd < 0)
    return -1;
  int n = mx_fio_lseek(fd, 0, MX_FIO_SEEK_END);
  if (n <= 0 || (u32)n > MAX_FILE || mx_fio_lseek(fd, 0, MX_FIO_SEEK_SET) != 0 ||
      mx_fio_read(fd, file_buf, (u32)n) != n)
    n = -1;
  mx_fio_close(fd);
  return n;
}

/* ---- shaders ---------------------------------------------------------------------- */

enum { S_FLAT, S_LIT, S_TEX, S_SKIN, S_N };
static const char *const shader_names[S_N] = {"MFLAT", "MLIT", "MTEX", "MSKIN"};
static struct mx_shader sh[S_N][2]; /* strip, indexed */
static const struct mx_shader *bound;

static void bind(const struct mx_shader *s) {
  if (bound != s) {
    mx_frame_shader_setup(s);
    bound = s;
  }
}

/* The streams of a shader's inputs; strips: the S* streams. */
static void streams_of(int shader, int strips, const void **st) {
  int o = strips ? MXM_SPOS - MXM_POS : 0;
  int k = 0;
  if (shader == S_SKIN) {
    st[k++] = sec(MXM_RPOS + o), st[k++] = sec(MXM_RNRM + o), st[k++] = sec(MXM_UV + o);
    st[k++] = sec(MXM_JNT + o), st[k++] = sec(MXM_WGT + o);
    return;
  }
  st[k++] = sec(MXM_POS + o);
  if (shader >= S_LIT)
    st[k++] = sec(MXM_NRM + o);
  if (shader >= S_TEX)
    st[k++] = sec(MXM_UV + o);
}

/* The vertices VU1 transforms for a strip of n drawn by mx_frame_shader_draw
   (its batches overlap by two vertices). */
static u32 strip_cost(const struct mx_shader *s, u32 n) {
  u32 size = (s->max_n - 2) / s->align * s->align + 2, step = size - 2, t = 0;
  for (u32 v = 0; v < n; v += step) {
    u32 k = n - v < size ? n - v : size;
    if (v && k < 3)
      break;
    t += k;
    if (v + k >= n)
      break;
  }
  return t;
}

/* ---- the view ------------------------------------------------------------------------ */

static struct {
  float yaw, pitch, dist, spin;
  int shader, indexed, copies, anim, turning, vsync;
} V = {0.6f, 0.25f, 0, 0, S_TEX, 1, 1, 0, 1, 1};

static float cam_c[3], cam_r[3], cam_u[3], cam_f[3];

/* The radius of the copies together. */
static float grid_radius(void) {
  return R + 2.4f * R * itof(V.copies - 1) * 0.5f * 1.4143f;
}
static float min_dist(void) { return grid_radius() * (1 + FX / GUARD); }

static void camera(void) {
  float cy = cosf_(V.yaw), sy = sinf_(V.yaw), cp = cosf_(V.pitch), sp = sinf_(V.pitch);
  cam_c[0] = V.dist * sy * cp, cam_c[1] = V.dist * sp, cam_c[2] = V.dist * cy * cp;
  cam_f[0] = -sy * cp, cam_f[1] = -sp, cam_f[2] = -cy * cp;
  cam_r[0] = cy, cam_r[1] = 0, cam_r[2] = -sy; /* f x up, normalized */
  cam_u[0] = -sy * sp, cam_u[1] = cp, cam_u[2] = -cy * sp;
}

/* The MVP columns of a copy at (ox, 0, oz): the model turned by V.spin around
   y, scaled to radius R around its centre; P / P.w are GS coordinates, P.z /
   P.w = ZK / depth. Also the light in model space. */
static void mvp_of(float m[4][4], float ox, float oz, float light[3]) {
  float c = cosf_(V.spin), s = sinf_(V.spin), k = R / M.h->radius;
  /* model -> world: rows of the rotation times k, then the move */
  float W[3][4] = {{c * k, 0, s * k, 0}, {0, k, 0, 0}, {-s * k, 0, c * k, 0}};
  for (int a = 0; a < 3; a++)
    W[a][3] = (a == 0 ? ox : a == 2 ? oz : 0) -
              (W[a][0] * M.h->center[0] + W[a][1] * M.h->center[1] + W[a][2] * M.h->center[2]);
  float A[4][3];
  for (int a = 0; a < 3; a++) {
    A[0][a] = 2048 * cam_f[a] + FX * cam_r[a];
    A[1][a] = 2048 * cam_f[a] - FY * cam_u[a];
    A[2][a] = 0;
    A[3][a] = cam_f[a];
  }
  for (int j = 0; j < 4; j++)
    for (int r = 0; r < 4; r++) {
      float v = 0;
      for (int a = 0; a < 3; a++)
        v += A[r][a] * (W[a][j] - (j == 3 ? cam_c[a] : 0));
      m[j][r] = v;
    }
  m[3][2] = ZK;
  /* the light (world: up and to the left of the view), turned back */
  static const float L[3] = {0.45f, 0.78f, 0.43f};
  light[0] = c * L[0] - s * L[2], light[1] = L[1], light[2] = s * L[0] + c * L[2];
}

/* ---- drawing ---------------------------------------------------------------------------- */

static float uni[VU_IX_MSKIN_UNI_QW + 4][4] MX_ALIGNED(16);
static u32 cur_mat = ~0u;
static struct {
  u32 verts, indices, tris, draws;
} st_frame;

/* GS state for material m (after the batches before it are drawn: a
   register written by PATH2 would reach the GS before their packets). */
static void gs_state(u32 m, int tex) {
  mx_frame_commit(mx_e_cnt(mx_frame_reserve(1), 0, mx_vif_flush(), 0));
  u32 w = 1u << M.mat[m].tw;
  u64 *c = mx_frame_direct(6);
  c = mx_e_ad_tag(c, 5, 1);
  c = mx_e_ad(c, MX_GS_REG_TEX0_1, mx_gs_tex0(M.tbp[m], w < 64 ? 1 : w / 64, MX_GS_PSM_CT32,
                                             M.mat[m].tw, M.mat[m].th, 1, 0));
  c = mx_e_ad(c, MX_GS_REG_TEX1_1, mx_gs_tex1(0, 0, 1, 1, 0, 0, 0));
  c = mx_e_ad(c, MX_GS_REG_CLAMP_1, mx_gs_clamp(0, 0, 0, 0, 0, 0));
  c = mx_e_ad(c, MX_GS_REG_TEST_1, mx_gs_test(0, 0, 0, 0, 0, 0, 1, 2));
  c = mx_e_ad(c, MX_GS_REG_RGBAQ, mx_gs_rgbaq(0x80, 0x80, 0x80, 0x80, 1.0f));
  mx_frame_commit(c);
  cur_mat = m;
  (void)tex;
}

/* The uniforms of shader for material m, palette p, frame of the
   animation. */
static void set_uniforms(int shader, const float mvp[4][4], const float light[3], u32 m, u32 p,
                         u32 frame) {
  for (int i = 0; i < 4; i++)
    for (int k = 0; k < 4; k++)
      uni[i][k] = mvp[i][k];
  const u8 *col = M.mat[m].color;
  if (shader == S_FLAT) {
    for (int k = 0; k < 3; k++)
      uni[VU_IX_MFLAT_COL][k] = (float)col[k];
    uni[VU_IX_MFLAT_COL][3] = 128;
    return;
  }
  /* lit: the light's colour and the ambient light, of the material's colour
     (untextured) or of 128 = 1.0 (modulating the texture) */
  float base[3];
  for (int k = 0; k < 3; k++)
    base[k] = shader == S_LIT ? (float)col[k] : 128.0f;
  float div = shader == S_SKIN ? 1.0f : 127.0f; /* mskin normalizes the normal */
  for (int k = 0; k < 3; k++) {
    uni[VU_IX_MLIT_LIGHT][k] = light[k] / div;
    uni[VU_IX_MLIT_LCOL][k] = base[k] * 0.85f;
    uni[VU_IX_MLIT_AMB][k] = base[k] * 0.32f;
  }
  uni[VU_IX_MLIT_LIGHT][3] = uni[VU_IX_MLIT_LCOL][3] = 0;
  uni[VU_IX_MLIT_AMB][3] = 128;
  if (shader != S_SKIN)
    return;
  const struct mxm_anim *a = &M.anim[V.anim];
  const float(*mats)[4] = (const void *)((const u8 *)sec(MXM_ANIM) + a->mat);
  mats += (frame % a->nframe) * M.h->njoint * 4;
  const struct mxm_pal *pl = &M.pal[p];
  for (u32 j = 0; j < pl->n; j++)
    for (int c = 0; c < 4; c++)
      for (int k = 0; k < 4; k++)
        uni[VU_IX_MSKIN_JOINT + 4 * j + c][k] = mats[4 * pl->joint[j] + c][k];
}

_Static_assert(VU_IX_MLIT_LIGHT == VU_IX_MTEX_LIGHT && VU_IX_MLIT_LIGHT == VU_IX_MSKIN_LIGHT &&
                   VU_IX_MLIT_LCOL == VU_IX_MSKIN_LCOL && VU_IX_MLIT_AMB == VU_IX_MSKIN_AMB,
               "the lit shaders share the uniforms' places");

static void draw_copy(float ox, float oz, u32 frame) {
  int shader = V.shader, tex = shader >= S_TEX;
  float m[4][4], light[3];
  mvp_of(m, ox, oz, light);
  const struct mx_shader *s = &sh[shader][V.indexed];
  u32 prim = (u32)mx_gs_prim(MX_GS_PRIM_TRISTRIP, 1, tex, 0, 0, 0, 0, 0, 0);
  const void *st[6];
  if (V.indexed) {
    streams_of(shader, 0, st);
    struct mx_ibatch *b = ibatch[s->out_qw == 3];
    for (u32 c = 0; c < M.h->ncluster;) {
      /* the clusters of one material and palette, one draw */
      u32 e = c + 1;
      while (e < M.h->ncluster && M.cl[e].mat == M.cl[c].mat &&
             (shader != S_SKIN || M.cl[e].pal == M.cl[c].pal))
        e++;
      if (tex && M.cl[c].mat != cur_mat)
        gs_state(M.cl[c].mat, 1);
      bind(s);
      set_uniforms(shader, m, light, M.cl[c].mat, M.cl[c].pal, frame);
      mx_dcache_writeback(uni, sizeof uni);
      mx_frame_shader_idraw(s, prim, uni, st, b + c, e - c);
      for (u32 k = c; k < e; k++)
        st_frame.verts += M.cl[k].nv, st_frame.indices += M.cl[k].ni;
      st_frame.draws++;
      c = e;
    }
  } else {
    for (u32 r = 0; r < M.h->nstrip; r++) {
      const struct mxm_strip *sr = &M.strip[r];
      streams_of(shader, 1, st);
      const void *at[6];
      for (u32 i = 0; i < s->nin; i++)
        at[i] = (const u8 *)st[i] + sr->first * s->in[i].bytes;
      if (tex && sr->mat != cur_mat)
        gs_state(sr->mat, 1);
      bind(s);
      set_uniforms(shader, m, light, sr->mat, sr->pal, frame);
      mx_dcache_writeback(uni, sizeof uni);
      mx_frame_shader_draw(s, prim, uni, at, sr->n);
      st_frame.verts += strip_cost(s, sr->n), st_frame.indices += sr->n;
      st_frame.draws++;
    }
  }
  st_frame.tris += M.h->ntri;
}

static char hud[3][96];

/* One frame of the view at time t (seconds). */
static void frame(float t) {
  V.dist = clampf(V.dist, min_dist(), 20 * grid_radius());
  camera();
  st_frame.verts = st_frame.indices = st_frame.tris = st_frame.draws = 0;
  mx_frame_begin(36, 40, 52);
  cur_mat = ~0u;
  u32 af = 0;
  if (V.shader == S_SKIN)
    af = (u32)(s32)(t * M.anim[V.anim].fps);
  float step = 2.4f * R, o = -step * itof(V.copies - 1) * 0.5f;
  for (int z = 0; z < V.copies; z++)
    for (int x = 0; x < V.copies; x++)
      draw_copy(o + step * itof(x), o + step * itof(z), af);
  mx_frame_commit(mx_e_cnt(mx_frame_reserve(1), 0, mx_vif_flush(), 0));
  for (int l = 0; l < 3; l++)
    hud_text(8, 6 + 9 * (u32)l, hud[l]);
  mx_frame_end();
}

/* ---- the pad ------------------------------------------------------------------------------- */

static u32 pad_pressed(void) { return mx_pad_pressed(0); }

/* ---- the run ------------------------------------------------------------------------------- */

static u8 ring[64 * 1024] MX_ALIGNED(64 * 1024);
#define FRAME_SPR ((void *)0x70002000)

static void pipeline_restart(void) {
  mx_frame_init(ring, sizeof ring, FRAME_SPR, 0x2000);
  mx_vu_forget(1);
  bound = 0;
}

static void hud_mode(char *p) {
  p = hud_cat(p, M.name);
  p = hud_cat(p, "  ");
  p = hud_cat(p, shader_names[V.shader]);
  p = hud_cat(p, V.indexed ? "  INDEXED  " : "  STRIPS  ");
  p = hud_u(p, (u32)(V.copies * V.copies));
  p = hud_cat(p, V.copies > 1 ? " COPIES" : " COPY");
  if (V.shader == S_SKIN) {
    p = hud_cat(p, "  ");
    char nm[17];
    for (int i = 0; i < 16; i++) {
      char ch = M.anim[V.anim].name[i];
      nm[i] = ch >= 'a' && ch <= 'z' ? (char)(ch - 32) : ch;
    }
    nm[16] = 0;
    hud_cat(p, nm);
  }
}
static void hud_counts(char *p) {
  p = hud_u(p, st_frame.tris);
  p = hud_cat(p, " TRIANGLES  ");
  p = hud_u(p, st_frame.verts);
  p = hud_cat(p, " VU1 VERTICES  ");
  p = hud_u(p, st_frame.indices);
  hud_cat(p, V.indexed ? " INDICES" : " STRIP");
}

/* The busy shares of the sampler (since mx_trace_sample was started). */
static void busy(u32 *vu1, u32 *path, u32 *gs) {
  u32 b[MX_TS_N], n = mx_trace_busy(b);
  *vu1 = n ? b[6] * 100 / n : 0, *path = n ? b[7] * 100 / n : 0, *gs = n ? b[8] * 100 / n : 0;
}

struct result {
  u32 shader, indexed, frames, cycles, tris, verts, indices, vu1, path, gs, ee;
};
static struct result results[2 * S_N];
static u32 nresults;

static void report(const struct result *r) {
  u32 fps10 = (u32)((u64)r->frames * MX_EE_HZ * 10 / r->cycles);
  u64 per_s = (u64)MX_EE_HZ * r->frames / r->cycles;
  mx_printf("model bench %-5s %-7s: %u.%u fps, %u triangles/s, %u VU1 vertices/s, %u %s/s; "
            "VU1 %u%% PATH %u%% GS %u%% EE %u%%\n",
            shader_names[r->shader], r->indexed ? "indexed" : "strips", fps10 / 10, fps10 % 10,
            (u32)(r->tris * per_s), (u32)(r->verts * per_s), (u32)(r->indices * per_s),
            r->indexed ? "indices" : "strip vertices", r->vu1, r->path, r->gs, r->ee);
}

static int want_interactive;

/* frames of the view as it is, without vsync: the counts a second */
static void bench(int shader, int indexed) {
  V.shader = shader, V.indexed = indexed;
  mx_frame_vsync(0);
  u32 waits0 = mx_frame_stats.wait_buffer + mx_frame_stats.wait_ring;
  mx_trace_sample(1000);
  u32 t0 = mx_count(), f0 = mx_gs.frames, v0 = mx_gs.vsync;
  for (u32 f = 0; f < BENCH_FRAMES; f++) {
    if (pad_pressed() & MX_PAD_START)
      want_interactive = 1;
    frame(itof((s32)(mx_gs.vsync - v0)) / 60);
    if (f == 0) {
      hud_mode(hud[0]);
      hud_counts(hud[1]);
    }
  }
  u32 d0 = mx_count();
  while (mx_gs.frames_done < f0 + BENCH_FRAMES && mx_count() - d0 < 300000000u)
    ;
  struct result *r = &results[nresults++];
  r->shader = (u32)shader, r->indexed = (u32)indexed, r->frames = BENCH_FRAMES;
  r->cycles = mx_count() - t0;
  r->tris = st_frame.tris, r->verts = st_frame.verts, r->indices = st_frame.indices;
  busy(&r->vu1, &r->path, &r->gs);
  mx_trace_sample(0);
  u32 w = mx_frame_stats.wait_buffer + mx_frame_stats.wait_ring - waits0;
  r->ee = w < r->cycles ? (u32)((u64)(r->cycles - w) * 100 / r->cycles) : 0;
  report(r);
  char *p = hud[2];
  p = hud_cat(p, "LAST: ");
  p = hud_u1(p, (u64)r->frames * MX_EE_HZ, r->cycles);
  p = hud_cat(p, " FPS  VU1 ");
  p = hud_u(p, r->vu1);
  hud_cat(p, "%");
}

/* The picture of the last frame: a hash of each line. */
static u32 line_hash[2][SH];
static int picture(u32 *hash) {
  static u32 line[SW] MX_ALIGNED(64);
  u32 last = mx_gs.frames - 1, d0 = mx_count();
  while (mx_gs.frames_done < last + 1 && mx_count() - d0 < 300000000u)
    ;
  mx_frame_stop();
  int r = 0;
  for (u32 y = 0; y < SH && !r; y++) {
    if (mx_gs_download(line, MX_GS_PAGE(mx_gs.fbp[last & 1]), mx_gs.fbw, MX_GS_PSM_CT32, 0, y, SW,
                       1) < 0)
      r = -1;
    u32 h = 2166136261u;
    for (u32 x = 0; x < SW; x++)
      h = (h ^ (line[x] & 0xFFFFFF)) * 16777619u;
    hash[y] = h;
  }
  pipeline_restart();
  return r;
}

/* One still frame of each shader as strips and indexed: the same picture. */
static int same_pictures(int nshader) {
  int fail = 0;
  mx_frame_vsync(1);
  V.copies = 2, V.spin = 0.7f, V.dist = 0;
  for (int s = 0; s < nshader; s++) {
    for (int ix = 0; ix < 2; ix++) {
      V.shader = s, V.indexed = ix;
      hud[0][0] = hud[1][0] = hud[2][0] = 0;
      frame(0.25f);
      if (picture(line_hash[ix]) < 0) {
        mx_printf("model: the picture could not be read\n");
        return 1;
      }
    }
    u32 diff = 0, first = 0;
    for (u32 y = 0; y < SH; y++)
      if (line_hash[0][y] != line_hash[1][y] && !diff++)
        first = y;
    if (diff) {
      mx_printf("model: %s strips and indexed differ in %u lines (the first %u)\n",
                shader_names[s], diff, first);
      fail = 1;
    } else {
      mx_printf("model: %s strips and indexed: the same picture\n", shader_names[s]);
    }
  }
  return fail;
}

static void interactive(int nshader) {
  mx_printf("model: interactive (SELECT + START ends)\n");
  mx_frame_vsync(V.vsync = 1);
  V.copies = 1;
  V.dist = 3.2f * min_dist();
  u32 v0 = mx_gs.vsync, n = 0, tp = mx_count(), fp = mx_gs.frames;
  u64 tri = 0, vert = 0;
  mx_trace_sample(1000);
  for (;;) {
    u32 b = pad_pressed(), held = mx_pad_buttons(0);
    if ((held & (MX_PAD_SELECT | MX_PAD_START)) == (MX_PAD_SELECT | MX_PAD_START))
      break;
    if (b & MX_PAD_SQUARE)
      V.shader = (V.shader + 1) % nshader;
    if (b & MX_PAD_CROSS)
      V.indexed ^= 1;
    if (b & MX_PAD_TRIANGLE) {
      V.copies = V.copies >= 16 ? 1 : V.copies * 2;
      V.dist = 3.2f * min_dist();
    }
    if ((b & MX_PAD_CIRCLE) && M.h->nanim)
      V.anim = (V.anim + 1) % (int)M.h->nanim;
    if (b & MX_PAD_L2)
      V.turning ^= 1;
    if (b & MX_PAD_R2)
      mx_frame_vsync(V.vsync ^= 1);
    u8 sticks[4];
    mx_pad_sticks(0, sticks);
    float lx = itof((s32)sticks[2] - 128) / 128, ly = itof((s32)sticks[3] - 128) / 128;
    float ry = itof((s32)sticks[1] - 128) / 128;
    lx = lx * lx < 0.02f ? 0 : lx, ly = ly * ly < 0.02f ? 0 : ly, ry = ry * ry < 0.02f ? 0 : ry;
    if (held & MX_PAD_LEFT)
      lx = -1;
    if (held & MX_PAD_RIGHT)
      lx = 1;
    if (held & MX_PAD_UP)
      ly = -1;
    if (held & MX_PAD_DOWN)
      ly = 1;
    if (held & MX_PAD_R1)
      ry = -1;
    if (held & MX_PAD_L1)
      ry = 1;
    V.yaw += lx * 0.04f;
    V.pitch = clampf(V.pitch - ly * 0.03f, -1.2f, 1.2f);
    V.dist *= 1 + ry * 0.03f;
    float t = itof((s32)(mx_gs.vsync - v0)) / 60;
    if (V.turning)
      V.spin = t * 0.6f;
    frame(t);
    tri += st_frame.tris, vert += st_frame.verts, n++;
    hud_mode(hud[0]);
    hud_counts(hud[1]);
    if (n == 60) {
      u32 cyc = mx_count() - tp, fr = mx_gs.frames - fp, vu1, path, gs;
      busy(&vu1, &path, &gs);
      char *p = hud[2];
      p = hud_u(p, (u32)((u64)fr * MX_EE_HZ / cyc));
      p = hud_cat(p, V.vsync ? " FPS  " : " FPS NO VSYNC  ");
      p = hud_u1(p, tri * MX_EE_HZ / cyc, 1000000);
      p = hud_cat(p, "M TRIANGLES/S  ");
      p = hud_u1(p, vert * MX_EE_HZ / cyc, 1000000);
      p = hud_cat(p, "M VERTICES/S  VU1 ");
      p = hud_u(p, vu1);
      p = hud_cat(p, "% GS ");
      p = hud_u(p, gs);
      hud_cat(p, "%");
      mx_trace_sample(0);
      mx_trace_sample(1000);
      tp = mx_count(), fp = mx_gs.frames, n = 0, tri = vert = 0;
    }
  }
  mx_trace_sample(0);
}

#ifdef MODEL_RECORD
#ifndef MODEL_RECORD_COPIES
#define MODEL_RECORD_COPIES 1
#endif
static void record(int skinned) {
  mx_frame_vsync(1);
  V.copies = MODEL_RECORD_COPIES, V.indexed = 1, V.shader = skinned ? S_SKIN : S_TEX;
  V.dist = 0;
  /* one: close; copies: the square of them about as wide as the screen */
  V.dist = V.copies > 1 ? 2.0f * grid_radius() : 1.6f * min_dist();
  V.pitch = V.copies > 1 ? 0.5f : 0.22f;
  for (u32 i = 0; i < MODEL_RECORD; i++) {
    float t = itof((s32)i) / 30;
    V.spin = t * 0.8f;
    /* the counts of the frame before: all frames draw the same */
    hud_mode(hud[0]);
    if (i)
      hud_counts(hud[1]);
    hud_u(hud_cat(hud[2], "FRAME "), i);
    frame(t);
    if (!i) {
      hud_counts(hud[1]);
      frame(t); /* the first again, with its counts */
    }
    /* the picture: once its frame is drawn, the pipeline stopped */
    u32 last = mx_gs.frames - 1, d0 = mx_count();
    while (mx_gs.frames_done < last + 1 && mx_count() - d0 < 300000000u)
      ;
    mx_frame_stop();
    char name[] = "host:rec_000.tga";
    name[9] = (char)('0' + i / 100 % 10), name[10] = (char)('0' + i / 10 % 10);
    name[11] = (char)('0' + i % 10);
    if (mx_gs_save_tga((int)(last & 1), name) < 0)
      mx_printf("model: %s could not be written\n", name);
    pipeline_restart();
  }
  mx_printf("model: %u frames recorded\n", (u32)MODEL_RECORD);
}
#endif

int main(int argc, char **argv) {
  mx_gs_init(SW, SH);
  mx_vu_reset(3);
  pipeline_restart();
  sh[S_FLAT][0] = MX_VU_SHADER(MFLAT, mflat_vu, mflat_vu_end);
  sh[S_FLAT][1] = MX_VU_ISHADER(IX_MFLAT, mflat_ix_vu, mflat_ix_vu_end);
  sh[S_LIT][0] = MX_VU_SHADER(MLIT, mlit_vu, mlit_vu_end);
  sh[S_LIT][1] = MX_VU_ISHADER(IX_MLIT, mlit_ix_vu, mlit_ix_vu_end);
  sh[S_TEX][0] = MX_VU_SHADER(MTEX, mtex_vu, mtex_vu_end);
  sh[S_TEX][1] = MX_VU_ISHADER(IX_MTEX, mtex_ix_vu, mtex_ix_vu_end);
  sh[S_SKIN][0] = MX_VU_SHADER(MSKIN, mskin_vu, mskin_vu_end);
  sh[S_SKIN][1] = MX_VU_ISHADER(IX_MSKIN, mskin_ix_vu, mskin_ix_vu_end);
  const struct mx_shader *ix[S_N] = {&sh[0][1], &sh[1][1], &sh[2][1], &sh[3][1]};

  /* the model: from the host, else the one built in */
  const u8 *base = model_fox;
  u32 size = (u32)(model_fox_end - model_fox);
  M.name = "FOX";
  if (argc > 1) {
    int n = model_read(argv[1]);
    if (n > 0) {
      base = file_buf, size = (u32)n, M.name = "MODEL";
      mx_printf("model: %s, %d bytes\n", argv[1], n);
    } else {
      mx_printf("model: %s could not be read, the fox instead\n", argv[1]);
    }
  }
  if (model_open(base, size, ix, S_N) < 0) {
    MX_TEST_END(1);
    return 1;
  }
  int skinned = (M.h->flags & MXM_SKINNED) && M.h->nanim && M.h->npal;
  int nshader = skinned ? S_N : S_SKIN;
  mx_printf("model: %u triangles, %u clusters, %u vertices in clusters, %u in strips, %u "
            "materials, %u joints, %u animations\n",
            M.h->ntri, M.h->ncluster, M.h->nvert, M.h->nsvert, M.h->nmat, M.h->njoint,
            M.h->nanim);
  for (u32 a = 0; a < M.h->nanim; a++)
    if (M.anim[a].name[0] == 'W' && M.anim[a].name[1] == 'a') /* Walk, if there is one */
      V.anim = (int)a;
  if (skinned && !V.anim && M.h->nanim > 1)
    V.anim = 1;
  hud_init(M.tbp_free, SH);
  if (mx_pad_init() < 0)
    mx_printf("model: no pad driver\n");
#ifdef MODEL_RECORD
  record(skinned);
  mx_frame_stop();
  MX_TEST_END(0);
  return 0;
#endif

  int fail = same_pictures(nshader);

  /* the counts a second of each shader, both ways */
  V.copies = BENCH_COPIES, V.spin = 0.3f, V.dist = 1.15f * min_dist();
  for (int s = 0; s < nshader && !want_interactive; s++)
    for (int i = 0; i < 2 && !want_interactive; i++)
      bench(s, i);
  if (!want_interactive) {
    mx_printf("model bench: %u x %u copies of %u triangles\n", BENCH_COPIES, BENCH_COPIES,
              M.h->ntri);
    for (u32 i = 0; i < nresults; i += 2)
      if (i + 1 < nresults)
        mx_printf("model bench %-5s: indexed / strips: %u%% of the time, %u%% of the VU1 "
                  "vertices\n",
                  shader_names[results[i].shader],
                  results[i].cycles ? (u32)((u64)results[i + 1].cycles * 100 / results[i].cycles)
                                    : 0,
                  results[i].verts ? results[i + 1].verts * 100 / results[i].verts : 0);
    /* a picture of the view */
    mx_frame_vsync(1);
    V.copies = 1, V.shader = skinned ? S_SKIN : S_TEX, V.indexed = 1, V.dist = 0;
    V.dist = 2.2f * min_dist();
    hud[2][0] = 0;
    for (int i = 0; i < 4; i++) {
      frame(0.5f);
      hud_mode(hud[0]);
      hud_counts(hud[1]);
    }
    u32 last = mx_gs.frames - 1, d0 = mx_count();
    while (mx_gs.frames_done < last + 1 && mx_count() - d0 < 300000000u)
      ;
    mx_frame_stop();
    if (mx_gs_save_tga((int)(last & 1), "host:model.tga") < 0)
      mx_printf("model: host:model.tga could not be written\n");
    pipeline_restart();
  }
  if (want_interactive)
    interactive(nshader);
  mx_frame_vsync(1);
  mx_frame_stop();
  mx_printf("model: %s\n", fail ? "FAILED" : "done");
  MX_TEST_END(fail);
  return fail;
}
