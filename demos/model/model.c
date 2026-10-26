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
     mscei  ps2gl's SCEI shader: three lights, clamped, textured
     msceic + its clipping: a triangle with a vertex beyond the guard band
            is not drawn (ADC); strips only (indexed, it is drawn as strips)
     mskin  + skinning: four joints a vertex, the animation baked by the
            converter (a skinned model only)

   A skinned model animates with every program: mskin skins on VU1, for the
   others the EE skins the vertices once a frame (VU0 macro mode) into the
   streams they read. -static: they draw the rest pose.

   A model with levels of detail (fbx2glb.py --lods) draws each part at the
   level its projected size asks for: level 0 down to a radius of R0 / 2
   pixels, one level more at each halving (-lodr <px>: R0, 512 by default;
   -lod <k>: always level k).

   The models: host:<argv[1]> .. (.mxm files, from ps2client's directory),
   else the one built in (Fox, demos/model/assets). The run is of the first;
   SELECT in the interactive mode goes to the next. -i (before or between
   them): the interactive mode at once; -aa: AA1 (the GS draws the edges of
   the triangles with their coverage); -soft: the display filter
   (mx_gs_smooth: half a pixel across, blended). R3: off, AA1, the filter,
   both. -cutout: the materials with alpha only cut out (alpha test), not
   blended. -nophys: no springs (MXM_PHYS: joints such as hair and cloth
   pulled towards the animation by springs, live, in world space).

   The run, without a pad: for each shader one frame as strips and one
   indexed, which must be the same picture (the outputs are the same, bit
   for bit; msceic: the same as mscei, nothing is beyond the guard band);
   then each shader both ways without vsync for BENCH_FRAMES frames,
   BENCH_COPIES x BENCH_COPIES copies (frames, triangles, vertices and
   indices a second, the units' busy shares); then a screenshot
   (host:model.tga). START on pad 0 during the run: the interactive mode.

   It starts with mscei, the model animated, and the camera on a flyby: it
   circles the model, rising and falling, nearer and farther (never nearer
   than the whole model fits the guard band). A stick, the D-pad, R1 or L1
   take the camera over where it is:

     left stick          walk (as Fortnite)       right stick         look around
     D-pad               turn, walk forwards/back  R1 / L1             up / down
     L3                  the flyby again
     SQUARE              next shader               CROSS              strips / indexed
     TRIANGLE            copies 1, 2x2 .. 16x16 (a single model: springs on/off)
     CIRCLE              next animation
     L2                  turning on/off            R2                 vsync on/off
     SELECT              next model                R3                 AA1 / filter / both / off
     SELECT + START      end

   Indexed, the view walks: the camera anywhere, from the eyes' height (R1 /
   L1 up and down), the clusters that are not all inside the guard band (their
   spheres, moved with their joints) left out, as the programs do not clip.
   Strips: the view turns around the model (right stick), never nearer than
   the whole model fits the guard band. The model turns by itself after L2.

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
#include "mscei.h"
#include "mscei_ix.h"
#include "msceic.h"
#include "mskin.h"
#include "mskin_ix.h"
#include "mtex.h"
#include "mtex_ix.h"
#include "mxm.h"

extern const u8 mflat_vu[], mflat_vu_end[], mlit_vu[], mlit_vu_end[], mtex_vu[], mtex_vu_end[],
    mscei_vu[], mscei_vu_end[], msceic_vu[], msceic_vu_end[], mskin_vu[], mskin_vu_end[],
    mflat_ix_vu[], mflat_ix_vu_end[], mlit_ix_vu[], mlit_ix_vu_end[], mtex_ix_vu[],
    mtex_ix_vu_end[], mscei_ix_vu[], mscei_ix_vu_end[], mskin_ix_vu[], mskin_ix_vu_end[];
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
_Static_assert(VU_MSCEI_A == VU_IX_MSCEI_A && VU_MSCEI_SCL == VU_IX_MSCEI_SCL &&
                   VU_MSCEIC_A == VU_IX_MSCEI_A && VU_MSCEIC_SCL == VU_IX_MSCEI_SCL,
               "");
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
static u32 atou_(const char *s) {
  u32 v = 0;
  while (*s >= '0' && *s <= '9')
    v = v * 10 + (u32)(*s++ - '0');
  return v;
}
static int strcmp_(const char *a, const char *b) {
  while (*a && *a == *b)
    a++, b++;
  return *a - *b;
}
static float itof(s32 v) { return (float)v; }

/* ---- the model -------------------------------------------------------------------- */

#define MAX_FILE (12u << 20)
#define MAX_CLUSTER 16384
#define MAX_MAT 16
#define MAX_PAL 256
/* GS memory in blocks (256 bytes), and the HUD's texture at the end of what
   the model's take: 256 x 8 CT32, 4 pages wide */
#define GS_BLOCKS (4u << 20 >> 8)
#define HUD_BLOCKS (4 * 32)
static u8 file_buf[MAX_FILE] MX_ALIGNED(64);
static u8 idx_buf[2][1u << 20] MX_ALIGNED(64); /* the indices times 2 and times 3 */
static struct mx_ibatch ibatch[2][MAX_CLUSTER];
/* of each cluster: the joint with the most weight (its sphere moves with it;
   0xFFFF: none), and its triangles (the strip without the joins) */
static u16 cl_joint[MAX_CLUSTER], cl_tris[MAX_CLUSTER];
/* its first vertices of one joint (weight 255 in the first slot), the ones
   of two after them (gltf2mxm.py puts them first, in that order): the EE
   skins them in loops of their own */
static u16 cl_n1[MAX_CLUSTER], cl_n2[MAX_CLUSTER];
/* The draws of the clusters: the runs of one material and level ([1]: and
   palette, mskin's), with their counts. */
#define MAX_GROUP 4096
static struct group {
  u32 c, e; /* clusters c .. e - 1 */
  u16 mat, pal;
  u32 nv, ni, tris;
} grp[2][MAX_GROUP];
static u32 ngrp[2];

static struct {
  const struct mxm_header *h;
  const u8 *base;
  const struct mxm_cluster *cl;
  const struct mxm_strip *strip;
  const struct mxm_mat *mat;
  const struct mxm_pal *pal;
  const struct mxm_anim *anim;
  u32 tbp[MAX_MAT], cbp[MAX_MAT]; /* the textures and the CLUTs (T8) in GS memory */
  u32 tbp_free; /* the first block after the textures */
  const char *name;
  int skinned; /* animations, a palette: mskin draws it */
  int ee_skin; /* and the EE can skin it for the other programs */
} M;

static const void *sec(int s) { return M.h->off[s] ? M.base + M.h->off[s] : 0; }

/* ---- levels of detail ------------------------------------------------------------------ */

/* A model may have levels of detail (gltf2mxm.py: mesh nodes <name>_LOD<k>,
   fbx2glb.py --lods): level k has about 2^-k of the triangles of each part.
   A cluster's or strip run's mat has the material in its low byte and the
   level in its high one. Each frame and copy, each material is drawn at one
   level: the one its projected size asks for, the same rule for every
   part: level 0 down to a radius of lod_r0 / 2 pixels, then one level more
   at each halving of the radius (-lodr <px>; -lod <k>: always level k). */
#define MAX_COPIES (16 * 16)
#define MAX_STRIP 4096
static u32 nlod;                /* levels of the model */
static float part[MAX_MAT][4];  /* each material's level 0: a sphere in model space */
static u32 run_tris[MAX_STRIP]; /* the triangles of each strip run */
static u8 lod_sel[MAX_COPIES][MAX_MAT];
static u16 lod_need[MAX_MAT];   /* the levels a copy draws this frame (bits) */
static int lod_force = -1;
static float lod_r0 = 512;

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
  if (h->ncluster > MAX_CLUSTER || h->nmat > MAX_MAT || !(h->ncluster || h->nstrip) || !h->nmat) {
    mx_printf("model: %u clusters, %u strips, %u materials\n", h->ncluster, h->nstrip, h->nmat);
    return -1;
  }
  M.cl = sec(MXM_CLUSTER), M.strip = sec(MXM_STRIP), M.mat = sec(MXM_MAT);
  M.pal = sec(MXM_PAL), M.anim = sec(MXM_ANIM);
  /* the clusters must fit every indexed program */
  for (u32 i = 0; i < nix && h->ncluster; i++)
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
        k->idx % 16 || (k->mat & 0xFF) >= h->nmat || (h->npal && k->pal >= h->npal)) {
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
  if (h->npal > MAX_PAL) {
    mx_printf("model: %u palettes\n", h->npal);
    return -1;
  }
  for (u32 p = 0; p < h->npal; p++)
    if (M.pal[p].n > JOINTS) {
      mx_printf("model: a palette of %u joints\n", M.pal[p].n);
      return -1;
    }
  const u8 *jnt = sec(MXM_JNT), *wgt = sec(MXM_WGT);
  for (u32 c = 0; c < h->ncluster; c++) {
    const struct mxm_cluster *k = &M.cl[c];
    const u8 *q = idx + k->idx;
    u32 t = 0;
    for (u32 i = 2; i < k->ni; i++)
      t += q[i] != q[i - 1] && q[i] != q[i - 2] && q[i - 1] != q[i - 2];
    cl_tris[c] = (u16)t;
    cl_joint[c] = 0xFFFF;
    cl_n1[c] = cl_n2[c] = 0;
    if (jnt && wgt && h->npal) {
      u32 v = k->first, e = k->first + k->nv;
      while (v < e && wgt[4 * v] == 255)
        v++;
      cl_n1[c] = (u16)(v - k->first);
      while (v < e && !wgt[4 * v + 2] && !wgt[4 * v + 3])
        v++;
      cl_n2[c] = (u16)(v - k->first - cl_n1[c]);
      u32 sum[MXM_PAL_MAX] = {0}, best = 0;
      for (u32 v = k->first; v < k->first + k->nv; v++)
        for (int i = 0; i < 4; i++) {
          if (jnt[4 * v + i] >= M.pal[k->pal].n) {
            mx_printf("model: cluster %u: a joint not in its palette\n", c);
            return -1;
          }
          sum[jnt[4 * v + i]] += wgt[4 * v + i];
        }
      for (u32 s = 1; s < MXM_PAL_MAX; s++)
        best = sum[s] > sum[best] ? s : best;
      cl_joint[c] = M.pal[k->pal].joint[best];
    }
  }
  for (int by_pal = 0; by_pal < 2; by_pal++) {
    u32 n = 0;
    for (u32 c = 0; c < h->ncluster;) {
      u32 e = c + 1;
      while (e < h->ncluster && M.cl[e].mat == M.cl[c].mat &&
             (!by_pal || M.cl[e].pal == M.cl[c].pal))
        e++;
      if (n == MAX_GROUP) {
        mx_printf("model: more than %u draws\n", MAX_GROUP);
        return -1;
      }
      struct group *g = &grp[by_pal][n++];
      *g = (struct group){c, e, M.cl[c].mat, M.cl[c].pal, 0, 0, 0};
      for (; c < e; c++)
        g->nv += M.cl[c].nv, g->ni += M.cl[c].ni, g->tris += cl_tris[c];
    }
    ngrp[by_pal] = n;
  }
  /* the levels; each material's sphere from its level 0 clusters (the
     model's without clusters); the triangles of each strip run: of its
     clusters (the same material, level and palette), else its length */
  if (h->nstrip > MAX_STRIP) {
    mx_printf("model: %u strip runs\n", h->nstrip);
    return -1;
  }
  nlod = 1;
  for (u32 c = 0; c < h->ncluster; c++)
    nlod = M.cl[c].mat >> 8 >= nlod ? (u32)(M.cl[c].mat >> 8) + 1 : nlod;
  for (u32 r = 0; r < h->nstrip; r++)
    nlod = M.strip[r].mat >> 8 >= nlod ? (u32)(M.strip[r].mat >> 8) + 1 : nlod;
  for (u32 m = 0; m < h->nmat; m++) {
    float c[3] = {0, 0, 0}, w = 0, r = 0;
    for (u32 k = 0; k < h->ncluster; k++)
      if (M.cl[k].mat == m) {
        for (int i = 0; i < 3; i++)
          c[i] += M.cl[k].sphere[i] * M.cl[k].nv;
        w += M.cl[k].nv;
      }
    for (int i = 0; i < 3; i++)
      part[m][i] = w > 0 ? c[i] / w : h->center[i];
    for (u32 k = 0; k < h->ncluster; k++)
      if (M.cl[k].mat == m) {
        const float *sp = M.cl[k].sphere;
        float d[3] = {sp[0] - part[m][0], sp[1] - part[m][1], sp[2] - part[m][2]};
        float e = __builtin_sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) + sp[3];
        r = e > r ? e : r;
      }
    part[m][3] = w > 0 ? r : h->radius;
  }
  for (u32 r = 0; r < h->nstrip; r++) {
    u32 t = 0;
    for (u32 c = 0; c < h->ncluster; c++)
      if (M.cl[c].mat == M.strip[r].mat && M.cl[c].pal == M.strip[r].pal)
        t += cl_tris[c];
    run_tris[r] = h->ncluster ? t : M.strip[r].n - 2;
  }
  /* an index is the vertex times the program's outputs (2 or 3) */
  for (u32 i = 0; i < idx_bytes; i++)
    idx_buf[0][i] = (u8)(idx[i] * 2), idx_buf[1][i] = (u8)(idx[i] * 3);
  mx_dcache_writeback(idx_buf, sizeof idx_buf);
  for (u32 c = 0; c < h->ncluster; c++)
    for (int o = 0; o < 2; o++)
      ibatch[o][c] = (struct mx_ibatch){M.cl[c].first, M.cl[c].nv, M.cl[c].ni,
                                        idx_buf[o] + M.cl[c].idx};
  /* the textures after the framebuffers, each from a page (T8 pages are
     128 x 64, CT32 ones 64 x 32); then the CLUTs, 4 blocks each */
  u32 tbp = mx_gs.tbp, nclut = 0;
  const u8 *tex = sec(MXM_TEX);
  for (u32 m = 0; m < h->nmat; m++) {
    const struct mxm_mat *mt = &M.mat[m];
    u32 w = 1u << mt->tw, hh = 1u << mt->th, t8 = mt->psm == MXM_TEX_T8;
    u32 pages = t8 ? (w < 128 ? 1 : w / 128) * ((hh + 63) / 64)
                   : (w < 64 ? 1 : w / 64) * ((hh + 31) / 32);
    if (tbp + pages * 32 > GS_BLOCKS - HUD_BLOCKS) {
      mx_printf("model: the textures do not fit the GS memory\n");
      return -1;
    }
    M.tbp[m] = tbp;
    if (t8) {
      M.cbp[m] = nclut++;
      mx_frame_texture(0, tbp, w < 128 ? 2 : w / 64, MX_GS_PSM_T8, w, hh, tex + mt->tex);
    } else {
      mx_frame_texture(0, tbp, w < 64 ? 1 : w / 64, MX_GS_PSM_CT32, w, hh, tex + mt->tex);
    }
    mx_frame_wait_path3();
    tbp += pages * 32;
  }
  if (tbp + (4 * nclut + 31) / 32 * 32 > GS_BLOCKS - HUD_BLOCKS) {
    mx_printf("model: the textures do not fit the GS memory\n");
    return -1;
  }
  for (u32 m = 0; m < h->nmat; m++)
    if (M.mat[m].psm == MXM_TEX_T8) {
      M.cbp[m] = tbp + 4 * M.cbp[m];
      mx_frame_texture(0, M.cbp[m], 1, MX_GS_PSM_CT32, 16, 16, tex + M.mat[m].clut);
      mx_frame_wait_path3();
    }
  M.tbp_free = tbp + (4 * nclut + 31) / 32 * 32;
  return 0;
}

/* host:path to dst, at most room bytes. Returns the size, or -1. */
static int model_read(const char *path, u8 *dst, u32 room) {
  int fd = mx_fio_open(path, MX_FIO_RDONLY);
  if (fd < 0)
    return -1;
  int n = mx_fio_lseek(fd, 0, MX_FIO_SEEK_END);
  if (n <= 0 || (u32)n > room || mx_fio_lseek(fd, 0, MX_FIO_SEEK_SET) != 0 ||
      mx_fio_read(fd, dst, (u32)n) != n)
    n = -1;
  mx_fio_close(fd);
  return n;
}

/* The models: the files of the arguments, one after the other in file_buf. */
#define MAX_MODELS 8
static struct {
  const u8 *base;
  u32 size;
  char name[24]; /* the file's name, as the HUD writes it */
} models[MAX_MODELS];
static u32 nmodel, cur_model;

static void model_add(const u8 *base, u32 size, const char *path) {
  const char *s = path;
  for (const char *p = path; *p; p++)
    if (*p == '/' || *p == ':')
      s = p + 1;
  char *d = models[nmodel].name;
  for (; *s && *s != '.' && d < models[nmodel].name + sizeof models[0].name - 1; s++)
    *d++ = *s >= 'a' && *s <= 'z' ? (char)(*s - 32) : *s == '_' ? ' ' : *s;
  *d = 0;
  models[nmodel].base = base, models[nmodel].size = size;
  nmodel++;
}

/* ---- shaders ---------------------------------------------------------------------- */

enum { S_FLAT, S_LIT, S_TEX, S_SCEI, S_SCEIC, S_SKIN, S_N };
static const char *const shader_names[S_N] = {"MFLAT", "MLIT",   "MTEX",
                                              "MSCEI", "MSCEIC", "MSKIN"};
static struct mx_shader sh[S_N][2]; /* strip, indexed */
/* the same, the position from the EE's skin_pos (V4-32) */
static struct mx_shader sh_ee[S_N][2];
/* msceic has no indexed program: indexed, it draws strips */
static int has_ix(int shader) { return shader != S_SCEIC; }
static const struct mx_shader *bound;

static void bind(const struct mx_shader *s) {
  if (bound != s) {
    mx_frame_shader_setup(s);
    bound = s;
  }
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
} V = {0.6f, 0.25f, 0, 0, S_SCEI, 1, 1, 0, 1, 1};

/* the view's shader draws indexed (msceic: strips, the view as indexed) */
static int ix_now(void) { return V.indexed && has_ix(V.shader); }

/* -static: the programs but mskin draw the rest pose (skinned once) */
static int static_pose;
/* shader draws the model animated: mskin skins on VU1, the others draw what
   the EE skinned */
static int animated(int shader) {
  return M.skinned && (shader == S_SKIN || !static_pose);
}

static float cam_c[3], cam_r[3], cam_u[3], cam_f[3];
/* AA1: the GS draws the edges of the triangles with their coverage as the
   alpha of (Cs - Cd) As + Cd */
static int aa1;
/* -cutout: the materials with alpha only alpha tested, not blended */
static int cutout;

/* The passes of a frame: the solid materials; then those with alpha
   (MXM_MAT_ALPHA), blended: their texels over half opaque, writing Z (the
   cards hide each other), then the rest, not writing Z (the soft edges hide
   nothing). */
enum { P_SOLID, P_BLEND, P_EDGE, P_N };
static int blended(u32 m) { return (M.mat[m].flags & MXM_MAT_ALPHA) && !cutout; }
/* The interactive view of the indexed programs flies: the camera at fly_c,
   looking along yaw and pitch. Else it turns around the model (at the
   origin) at V.dist. */
static int fly;
static float fly_c[3];
/* the interactive mode's flyby: the camera circles the model by itself */
static int flyby;
/* the nearest depth of a cluster drawn, in world units */
#define NEAR_Z 1.0f
/* walking: the height of the eyes and the step a frame, world units (the
   model's radius is R) */
#define EYE_Y (0.8f * R)
#define WALK (0.03f * R)

/* n x n copies, or one of a model made to be drawn once (MXM_SINGLE). */
static int copies(int n) { return M.h->flags & MXM_SINGLE ? 1 : n; }

/* The radius of the copies together. */
static float grid_radius(void) {
  return R + 2.4f * R * itof(V.copies - 1) * 0.5f * 1.4143f;
}
static float min_dist(void) { return grid_radius() * (1 + FX / GUARD); }

static void camera(void) {
  float cy = cosf_(V.yaw), sy = sinf_(V.yaw), cp = cosf_(V.pitch), sp = sinf_(V.pitch);
  if (fly)
    cam_c[0] = fly_c[0], cam_c[1] = fly_c[1], cam_c[2] = fly_c[2];
  else
    cam_c[0] = V.dist * sy * cp, cam_c[1] = V.dist * sp, cam_c[2] = V.dist * cy * cp;
  cam_f[0] = -sy * cp, cam_f[1] = -sp, cam_f[2] = -cy * cp;
  cam_r[0] = cy, cam_r[1] = 0, cam_r[2] = -sy; /* f x up, normalized */
  cam_u[0] = -sy * sp, cam_u[1] = cp, cam_u[2] = -cy * sp;
}

/* A direction of the world in model space (the model turned by V.spin). */
static void turn_back(const float *w, float *m) {
  float c = cosf_(V.spin), s = sinf_(V.spin);
  m[0] = c * w[0] - s * w[2], m[1] = w[1], m[2] = s * w[0] + c * w[2];
}

/* The MVP columns of a copy at (ox, 0, oz): the model turned by V.spin around
   y, scaled to radius R around its centre; P / P.w are GS coordinates, P.z /
   P.w = ZK / depth. Also the light and the eye in model space. */
static void mvp_of(float m[4][4], float ox, float oz, float light[3], float eye[3]) {
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
  turn_back(L, light);
  /* the eye: world -> model is the inverse of W (its rows are a rotation
     times k) */
  for (int a = 0; a < 3; a++) {
    float v = 0;
    for (int b = 0; b < 3; b++)
      v += W[b][a] * (cam_c[b] - W[b][3]);
    eye[a] = v / (k * k);
  }
}

/* ---- drawing ---------------------------------------------------------------------------- */

static float uni[VU_IX_MSKIN_UNI_QW + 4][4] MX_ALIGNED(16);
static u32 cur_mat = ~0u;
static struct {
  u32 verts, indices, tris, draws;
  u32 skin, skinned; /* EE cycles of the skinning, vertices skinned */
} st_frame;

/* GS state for material m in pass (after the batches before it are drawn:
   a register written by PATH2 would reach the GS before their packets). */
static void gs_state(u32 m, int pass) {
  mx_frame_commit(mx_e_cnt(mx_frame_reserve(1), 0, mx_vif_flush(), 0));
  const struct mxm_mat *mt = &M.mat[m];
  u32 w = 1u << mt->tw;
  u64 tex0 = mt->psm == MXM_TEX_T8
                 ? mx_gs_tex0(M.tbp[m], w < 128 ? 2 : w / 64, MX_GS_PSM_T8, mt->tw, mt->th, 1, 3) |
                       MX_SET(GS_TEX0_CBP, M.cbp[m]) | MX_SET(GS_TEX0_CLD, 1)
                 : mx_gs_tex0(M.tbp[m], w < 64 ? 1 : w / 64, MX_GS_PSM_CT32, mt->tw, mt->th, 1, 3);
  /* TFX HIGHLIGHT2: the texture times the colour, plus the colour's alpha
     (the highlight); the alpha drawn is the texture's */
  u64 *c = mx_frame_direct(7);
  c = mx_e_ad_tag(c, 6, 1);
  c = mx_e_ad(c, MX_GS_REG_TEX0_1, tex0);
  c = mx_e_ad(c, MX_GS_REG_TEX1_1, mx_gs_tex1(0, 0, 1, 1, 0, 0, 0));
  c = mx_e_ad(c, MX_GS_REG_CLAMP_1, mx_gs_clamp(0, 0, 0, 0, 0, 0));
  /* the alpha test: texels over half opaque (solid, and the blended pass
     that writes Z), or the others (the soft edges) */
  c = mx_e_ad(c, MX_GS_REG_TEST_1,
              mx_gs_test(1, pass == P_EDGE ? 3 : 6, 0x40, 0, 0, 0, 1, 2));
  c = mx_e_ad(c, MX_GS_REG_ZBUF_1, mx_gs_zbuf(mx_gs.zbp, MX_GS_PSM_Z24, pass == P_EDGE));
  c = mx_e_ad(c, MX_GS_REG_RGBAQ, mx_gs_rgbaq(0x80, 0x80, 0x80, 0x80, 1.0f));
  mx_frame_commit(c);
  cur_mat = m | (u32)pass << 8;
}

/* The joint matrices (four columns each) of frame of the animation. */
static const float (*anim_mats_of(u32 anim, u32 frame))[4] {
  const struct mxm_anim *a = &M.anim[anim];
  const float(*mats)[4] = (const void *)((const u8 *)sec(MXM_ANIM) + a->mat);
  return mats + (frame % a->nframe) * M.h->njoint * 4;
}
static const float (*anim_mats(u32 frame))[4] { return anim_mats_of((u32)V.anim, frame); }

/* ---- springs (MXM_PHYS) ----------------------------------------------------------- */

#define MAX_PHYS 192
#define MAX_JOINTS 1024
#define MAX_MOVED 512
#define MAX_COLL 32
/* -nophys, or TRIANGLE (a single model): the animation as it is */
static int phys_off;
static struct {
  int n, active, reset;  /* records; this frame's matrices moved; start again */
  int ncoll;
  float ca[MAX_COLL][3], cab[MAX_COLL][3], cil2[MAX_COLL], cr[MAX_COLL]; /* this frame's
      capsules (world): a, b - a, 1 / |b - a|^2, the radius */
  float t;               /* the time of the last step */
  float x[MAX_PHYS][3], v[MAX_PHYS][3]; /* the tips: world space (turned with the model) */
  float tp[MAX_PHYS][3];                 /* where the animation put them the step before */
  s16 slot[MAX_JOINTS];  /* of a joint in mat, -1: not moved */
  float mat[MAX_MOVED][4][4] MX_ALIGNED(16); /* the moved joints' matrices (four columns) */
} P;

/* The springs of the model: the joints they move get a slot. */
static void phys_open(void) {
  const u32 *sec_ = sec(MXM_PHYS);
  P.n = 0, P.active = 0, P.reset = 1;
  if (!sec_ || !(M.h->flags & MXM_SKINNED) || M.h->njoint > MAX_JOINTS || sec_[0] > MAX_PHYS)
    return;
  for (u32 j = 0; j < M.h->njoint; j++)
    P.slot[j] = -1;
  const struct mxm_phys *ph = (const void *)(sec_ + 4);
  int nslot = 0;
  for (u32 i = 0; i < sec_[0]; i++) {
    const u16 *d = (const void *)((const u8 *)sec_ + ph[i].desc);
    for (int k = -1; k < (int)ph[i].ndesc; k++) {
      u32 j = k < 0 ? ph[i].joint : d[k];
      if (j >= M.h->njoint)
        return;
      if (P.slot[j] < 0) {
        if (nslot == MAX_MOVED)
          return;
        P.slot[j] = (s16)nslot++;
      }
    }
  }
  P.n = (int)sec_[0];
  const u32 *co = sec(MXM_COLL);
  P.ncoll = co && co[0] <= MAX_COLL ? (int)co[0] : 0;
}

static float sqrtf_(float x) { return __builtin_sqrtf(x); }
static float dot3(const float *a, const float *b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
/* The squared distance of x from capsule c's axis, and the closest point on
   it. */
static float capsule_dist2(int c, const float *x, float *q) {
  const float *a = P.ca[c], *ab = P.cab[c];
  float ax[3] = {x[0] - a[0], x[1] - a[1], x[2] - a[2]};
  float t = dot3(ax, ab) * P.cil2[c];
  t = t < 0 ? 0 : t > 1 ? 1 : t;
  float d[3];
  for (int k = 0; k < 3; k++)
    q[k] = a[k] + ab[k] * t, d[k] = x[k] - q[k];
  return dot3(d, d);
}

/* The capsules a spring may reach this frame, each with how far out it
   pushes the tip and the middle of the segment: its radius plus the
   spring's thickness, but not further out than where the animation has them
   (what the animation puts in a capsule may stay as deep). */
struct reach {
  int n;
  u8 c[MAX_COLL];
  float r2[2][MAX_COLL]; /* squared: the tip's, the middle's */
};

/* The move that takes x out of the capsules of re (k: 0 the tip, 1 the
   middle). */
static void capsule_push(const float *x, const struct reach *re, int k, float *push) {
  push[0] = push[1] = push[2] = 0;
  for (int i = 0; i < re->n; i++) {
    float q[3], r2 = re->r2[k][i], d2 = capsule_dist2(re->c[i], x, q);
    if (d2 >= r2 || d2 < 1e-12f)
      continue;
    float d = sqrtf_(d2), f = (sqrtf_(r2) - d) / d;
    for (int j = 0; j < 3; j++)
      push[j] += (x[j] - q[j]) * f;
  }
}

/* model space -> world: turned by the angle of cosine cs and sine sn about y */
static void to_world(float cs, float sn, const float *m, float *w) {
  w[0] = cs * m[0] + sn * m[2], w[1] = m[1], w[2] = -sn * m[0] + cs * m[2];
}

/* the point p moved by matrix m (four columns) */
static void xform(const float (*m)[4], const float *p, float *o) {
  for (int k = 0; k < 3; k++)
    o[k] = m[0][k] * p[0] + m[1][k] * p[1] + m[2][k] * p[2] + m[3][k];
}

/* This frame's joint matrices with the springs, at time t (seconds): each
   tip pulled towards where the animation puts it, in sub-steps of 1/240 s,
   in world space so that the model's turning moves it; the joint and its
   descendants turned about the head towards it. */
static void phys_update(u32 frame, float t) {
  P.active = 0;
  if (!P.n || phys_off || !animated(V.shader))
    return;
  const u32 *sec_ = sec(MXM_PHYS);
  const struct mxm_phys *ph = (const void *)(sec_ + 4);
  const float(*anim)[4] = anim_mats(frame);
  for (u32 j = 0; j < M.h->njoint; j++)
    if (P.slot[j] >= 0)
      for (int c = 0; c < 4; c++)
        for (int k = 0; k < 4; k++)
          P.mat[P.slot[j]][c][k] = anim[4 * j + c][k];
  /* model -> world (turned by V.spin about y, as mvp_of) and back */
  float cs = cosf_(V.spin), sn = sinf_(V.spin);
  const struct mxm_coll *co = (const void *)((const u32 *)sec(MXM_COLL) + 4);
  for (int c = 0; c < P.ncoll; c++) {
    float am[3], bm[3];
    xform(anim + 4 * co[c].joint, co[c].a, am);
    xform(anim + 4 * co[c].joint, co[c].b, bm);
    float bw[3];
    to_world(cs, sn, am, P.ca[c]);
    to_world(cs, sn, bm, bw);
    for (int k = 0; k < 3; k++)
      P.cab[c][k] = bw[k] - P.ca[c][k];
    float l2 = dot3(P.cab[c], P.cab[c]);
    P.cil2[c] = l2 > 1e-12f ? 1 / l2 : 0;
    P.cr[c] = co[c].radius;
  }
  float dt = t - P.t;
  int reset = P.reset || dt < 0 || dt > 0.25f, steps = reset ? 0 : (int)(dt * 240) + 1;
  P.t = t, P.reset = 0;
  float h = steps ? dt / itof(steps) : 0;
  for (int i = 0; i < P.n; i++) {
    const struct mxm_phys *r = &ph[i];
    float(*m)[4] = P.mat[P.slot[r->joint]];
    float hm[3], tm[3], hw[3], tw[3], a[3], b[3], u[3];
    xform((const void *)m, r->head, hm);
    xform((const void *)m, r->tip, tm);
    for (int k = 0; k < 3; k++)
      a[k] = tm[k] - hm[k];
    float len = sqrtf_(dot3(a, a));
    if (len < 1e-6f)
      continue;
    to_world(cs, sn, hm, hw);
    to_world(cs, sn, tm, tw);
    float *x = P.x[i], *v = P.v[i], *tp = P.tp[i], tv[3];
    if (reset)
      for (int k = 0; k < 3; k++)
        x[k] = tw[k], v[k] = 0, tp[k] = tw[k];
    /* the damping slows the tip's speed relative to the animation's (what
       moves with the body is not held back, as it would be by air) */
    for (int k = 0; k < 3; k++)
      tv[k] = steps ? (tw[k] - tp[k]) / dt : 0, tp[k] = tw[k];
    for (int k = 0; k < 3; k++)
      a[k] = (tw[k] - hw[k]) / len;
    float cl = cosf_(r->limit), sl = sinf_(r->limit), ca = 1;
    /* the middle of the segment as the animation has it */
    float mref[3] = {(hw[0] + tw[0]) / 2, (hw[1] + tw[1]) / 2, (hw[2] + tw[2]) / 2};
    /* the capsules in reach: the tip is put back at len from the head after
       each step; before the first it is where the last frame left it, and a
       step moves it by its speed (and the pull towards tw) */
    struct reach re;
    float xh[3] = {x[0] - hw[0], x[1] - hw[1], x[2] - hw[2]}, far = sqrtf_(dot3(xh, xh));
    far = (far > len ? far : len) + sqrtf_(dot3(v, v)) * dt + len;
    re.n = 0;
    for (int c = 0; c < P.ncoll; c++) {
      float q[3], rr = P.cr[c] + r->radius, reach = rr + far;
      if (capsule_dist2(c, hw, q) >= reach * reach)
        continue;
      float r2 = rr * rr, dt2 = capsule_dist2(c, tw, q), dm2 = capsule_dist2(c, mref, q);
      re.c[re.n] = (u8)c;
      re.r2[0][re.n] = dt2 < r2 ? dt2 : r2;
      re.r2[1][re.n++] = dm2 < r2 ? dm2 : r2;
    }
    for (int st = 0; st < steps || st == 0; st++) {
      float push[3];
      if (steps)
        for (int k = 0; k < 3; k++) {
          float acc =
              r->stiff * (tw[k] - x[k]) - r->damp * (v[k] - tv[k]) - (k == 1 ? r->gravity : 0);
          v[k] += acc * h;
          x[k] += v[k] * h;
        }
      /* out of the capsules: the tip, and the middle of the segment (moving
         the tip moves it half as far); no more speed into the capsule */
      if (re.n) {
        float mp[3];
        capsule_push(x, &re, 0, push);
        float mid[3] = {(hw[0] + x[0] + push[0]) / 2, (hw[1] + x[1] + push[1]) / 2,
                        (hw[2] + x[2] + push[2]) / 2};
        capsule_push(mid, &re, 1, mp);
        for (int k = 0; k < 3; k++)
          push[k] += 2 * mp[k];
        float pl = sqrtf_(dot3(push, push));
        if (pl > 1e-7f) {
          for (int k = 0; k < 3; k++)
            x[k] += push[k], push[k] /= pl;
          float vn = dot3(v, push);
          if (vn < 0)
            for (int k = 0; k < 3; k++)
              v[k] -= vn * push[k];
        }
      }
      /* at the joint's length from the head, within limit of the animation */
      for (int k = 0; k < 3; k++)
        b[k] = x[k] - hw[k];
      float bl = sqrtf_(dot3(b, b));
      if (bl < 1e-6f)
        for (int k = 0; k < 3; k++)
          b[k] = a[k];
      else
        for (int k = 0; k < 3; k++)
          b[k] /= bl;
      ca = dot3(a, b);
      if (ca < cl) {
        for (int k = 0; k < 3; k++)
          u[k] = b[k] - a[k] * ca;
        float ul = sqrtf_(dot3(u, u));
        for (int k = 0; k < 3; k++)
          b[k] = ul > 1e-6f ? a[k] * cl + u[k] / ul * sl : a[k];
        ca = cl;
      }
      for (int k = 0; k < 3; k++)
        x[k] = hw[k] + b[k] * len;
      /* a pendulum: no speed along the joint; slowed where the limit holds it */
      float vb = dot3(v, b);
      for (int k = 0; k < 3; k++)
        v[k] = (v[k] - vb * b[k]) * (ca <= cl ? 0.5f : 1.0f);
    }
    if (ca > 0.99999f)
      continue;
    /* back to model space, and the turn from a to b (Rodrigues): R = I + K +
       K K / (1 + a.b), K the cross product with a x b (rot) */
    float am[3] = {cs * a[0] - sn * a[2], a[1], sn * a[0] + cs * a[2]};
    float bm[3] = {cs * b[0] - sn * b[2], b[1], sn * b[0] + cs * b[2]};
    float w[3] = {am[1] * bm[2] - am[2] * bm[1], am[2] * bm[0] - am[0] * bm[2],
                  am[0] * bm[1] - am[1] * bm[0]};
    float K[3][3] = {{0, -w[2], w[1]}, {w[2], 0, -w[0]}, {-w[1], w[0], 0}}, rot[3][3];
    float f = 1 / (1 + ca);
    for (int p = 0; p < 3; p++)
      for (int q = 0; q < 3; q++)
        rot[p][q] = (p == q) + K[p][q] +
                  f * (K[p][0] * K[0][q] + K[p][1] * K[1][q] + K[p][2] * K[2][q]);
    float tr[3];
    for (int p = 0; p < 3; p++)
      tr[p] = hm[p] - (rot[p][0] * hm[0] + rot[p][1] * hm[1] + rot[p][2] * hm[2]);
    /* the joint and its descendants: about the head */
    const u16 *d = (const void *)((const u8 *)sec_ + r->desc);
    for (int k = -1; k < (int)r->ndesc; k++) {
      float(*o)[4] = P.mat[P.slot[k < 0 ? r->joint : d[k]]];
      for (int c = 0; c < 4; c++) {
        float col[3] = {o[c][0], o[c][1], o[c][2]};
        for (int p = 0; p < 3; p++)
          o[c][p] = rot[p][0] * col[0] + rot[p][1] * col[1] + rot[p][2] * col[2] + tr[p] * o[c][3];
      }
    }
  }
  P.active = 1;
}

/* ---- skinning on the EE ------------------------------------------------------------- */

/* Every program animates a skinned model, as on a PC the compute pass that
   skins once a frame for all shaders: the EE moves the bind pose (RPOS,
   RNRM; SRPOS, SRNRM as strips) by the joints (JNT, WGT) into skin_pos (V4-32:
   one SQC2, the programs unpack it as V4-32 instead of POS's V3-32,
   sh_ee) and skin_nrm (NRM's V4-8); mskin still skins on VU1 itself. Only
   the layout drawn, each vertex once; with VU0 in macro mode (-mvu0): the
   joint matrices blended by the weights (the vertices of one joint and of
   two in loops of their own), the position and the normal moved by them,
   the normal made 127 long again.
   Frame n uses buffer n & 1: frame n - 1 may still be drawn from the other
   (frame n - 2 is done once frame n has begun). -static: the rest pose (the
   first animation), skinned once into each buffer. */
#define MAX_SKIN (256u << 10)
typedef float f32x4 __attribute__((vector_size(16)));
typedef s32 s32x4 __attribute__((vector_size(16)));
typedef u8 u8x4 __attribute__((vector_size(4)));
typedef s8 s8x4 __attribute__((vector_size(4)));
static f32x4 skin_pos[2][MAX_SKIN + 8] MX_ALIGNED(64);
static s8x4 skin_nrm[2][MAX_SKIN + 8] MX_ALIGNED(64);
static int skin_buf;     /* this frame's */
static int skin_rest[2]; /* a buffer holds the rest pose of layout 1 + indexed, else 0 */

/* This frame's joint matrices (four columns), the springs' where they move
   a joint, with the bind pose's V3-16 folded in (mxm.h qsize, qoff): joint
   j moves the position q / 32768 (ITOF15) to jq[j] (q / 32768, 1) (mskin's
   uniforms). The EE's, je: jq with the 1 / 32768 in its columns and the
   1 / 65536 of the way it reads q (skin_pos_in). */
static f32x4 jq[MAX_JOINTS][4] MX_ALIGNED(16);
static f32x4 je[MAX_JOINTS][4] MX_ALIGNED(16);
/* je of each palette's joints, by slot: made when a frame first skins
   with the palette (jp_frame: the frame's joints_frame) */
static f32x4 jp[MAX_PAL][JOINTS][4] MX_ALIGNED(16);
static u32 jp_frame[MAX_PAL], joints_frames;

static void joints_frame(u32 anim, u32 frame) {
  const float(*mats)[4] = anim_mats_of(anim, frame);
  const struct mxm_header *h = M.h;
  for (u32 j = 0; j < h->njoint; j++) {
    const f32x4 *c = P.active && P.slot[j] >= 0 ? (const f32x4 *)P.mat[P.slot[j]]
                                                 : (const f32x4 *)(mats + 4 * j);
    jq[j][0] = c[0] * h->qsize, jq[j][1] = c[1] * h->qsize, jq[j][2] = c[2] * h->qsize;
    jq[j][3] = c[0] * h->qoff[0] + c[1] * h->qoff[1] + c[2] * h->qoff[2] + c[3];
    for (int k = 0; k < 3; k++)
      je[j][k] = jq[j][k] * (1.0f / 32768.0f / 65536.0f);
    je[j][3] = jq[j][3];
  }
  joints_frames++;
}

static const f32x4 (*pal_joints(u32 p))[4] {
  if (jp_frame[p] != joints_frames) {
    jp_frame[p] = joints_frames;
    for (u32 k = 0; k < JOINTS; k++) {
      const f32x4 *c = je[M.pal[p].joint[k < M.pal[p].n ? k : 0]];
      jp[p][k][0] = c[0], jp[p][k][1] = c[1], jp[p][k][2] = c[2], jp[p][k][3] = c[3];
    }
  }
  return jp[p];
}

/* The vertex loops of skin_run: one of each kind of vertex, without
   branches, two vertices an iteration so that the EE has the second's work
   to do while it waits for the first's results. Not software pipelined:
   it is not faster here, and PCSX2's EE recompiler runs the pipelined loop
   of one joint without normals wrong (docs/llvm-issues.md 13). */
#define SKIN_LOOP _Pragma("clang loop unroll_count(2) pipeline(disable)")
typedef s16 s16x8 __attribute__((vector_size(16)));
typedef s8 s8x16 __attribute__((vector_size(16)));
typedef u64 u64x2 __attribute__((vector_size(16)));
/* RPOS q times 65536 (w: the next one's x): LDL, LDR, PEXTLH with 0 */
MX_INLINE f32x4 skin_pos_in(const s16 *p) {
  u64 v;
  __builtin_memcpy(&v, p, 8);
  s16x8 h = (s16x8)(u64x2){v, 0}, z = {0};
  return __builtin_convertvector((s32x4)__builtin_shufflevector(z, h, 0, 8, 1, 9, 2, 10, 3, 11),
                                 f32x4);
}
/* RNRM times 2^24 (skin_unit makes it 127 long): LW, PEXTLB and PEXTLH
   with 0 */
MX_INLINE f32x4 skin_nrm_in(const s8x4 *n) {
  s8x16 b = (s8x16)(s32x4){*(const s32 *)n, 0, 0, 0}, z = {0};
  s16x8 h = (s16x8)__builtin_shufflevector(z, b, 0, 16, 1, 17, 2, 18, 3, 19, 4, 20, 5, 21, 6, 22,
                                           7, 23),
        hz = {0};
  return __builtin_convertvector((s32x4)__builtin_shufflevector(hz, h, 0, 8, 1, 9, 2, 10, 3, 11),
                                 f32x4);
}
MX_INLINE s8x4 skin_nrm_out(f32x4 t) {
  return __builtin_convertvector(__builtin_convertvector(t, s32x4), s8x4);
}
/* t at length 127 */
MX_INLINE f32x4 skin_unit(f32x4 t) {
  f32x4 t2 = t * t;
  return t * (127.0f / __builtin_sqrtf(t2[0] + t2[1] + t2[2] + 1e-30f));
}

/* n vertices of the streams pos .. wgt by the joints pm of their palette
   (jp) into op and on: the first n1 of one joint (the first slot's), the n2
   after them of two (the first two slots'), the rest of up to four (a slot
   of weight 0 is a slot of the palette too); normals: the normals too
   (mflat has none). */
MX_INLINE void skin_vertices(const s16 *__restrict pos, const s8x4 *__restrict nrm,
                             const u32 *__restrict jnt, const u8x4 *__restrict wgt,
                             const f32x4 (*__restrict pm)[4], f32x4 *__restrict op,
                             s8x4 *__restrict on, u32 n1, u32 n2, u32 n, const int normals) {
  u32 i = 0;
  /* one joint: its matrix as it is (the normal made 127 long as in the
     other loops: strip runs, not sorted, take theirs for all vertices) */
  SKIN_LOOP
  for (; i < n1; i++) {
    const f32x4 *m = pm[jnt[i] & 255];
    f32x4 p = skin_pos_in(pos + 3 * i);
    f32x4 r = m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3];
    op[i] = r;
    if (normals) {
      f32x4 nv = skin_nrm_in(nrm + i);
      on[i] = skin_nrm_out(skin_unit(m[0] * nv[0] + m[1] * nv[1] + m[2] * nv[2]));
    }
  }
  /* two: each column one sum of two products */
  SKIN_LOOP
  for (; i < n1 + n2; i++) {
    u32 jb = jnt[i];
    const f32x4 *a = pm[jb & 255], *b = pm[jb >> 8 & 255];
    f32x4 w = __builtin_convertvector(wgt[i], f32x4) * (1.0f / 255.0f);
    f32x4 c0 = a[0] * w[0] + b[0] * w[1], c1 = a[1] * w[0] + b[1] * w[1];
    f32x4 c2 = a[2] * w[0] + b[2] * w[1], c3 = a[3] * w[0] + b[3] * w[1];
    f32x4 p = skin_pos_in(pos + 3 * i);
    f32x4 r = c0 * p[0] + c1 * p[1] + c2 * p[2] + c3;
    op[i] = r;
    if (normals) {
      f32x4 nv = skin_nrm_in(nrm + i);
      on[i] = skin_nrm_out(skin_unit(c0 * nv[0] + c1 * nv[1] + c2 * nv[2]));
    }
  }
  /* up to four */
  SKIN_LOOP
  for (; i < n; i++) {
    u32 jb = jnt[i];
    const f32x4 *a = pm[jb & 255], *b = pm[jb >> 8 & 255];
    const f32x4 *c = pm[jb >> 16 & 255], *d = pm[jb >> 24];
    f32x4 w = __builtin_convertvector(wgt[i], f32x4) * (1.0f / 255.0f);
    f32x4 c0 = a[0] * w[0] + b[0] * w[1] + c[0] * w[2] + d[0] * w[3];
    f32x4 c1 = a[1] * w[0] + b[1] * w[1] + c[1] * w[2] + d[1] * w[3];
    f32x4 c2 = a[2] * w[0] + b[2] * w[1] + c[2] * w[2] + d[2] * w[3];
    f32x4 c3 = a[3] * w[0] + b[3] * w[1] + c[3] * w[2] + d[3] * w[3];
    f32x4 p = skin_pos_in(pos + 3 * i);
    f32x4 r = c0 * p[0] + c1 * p[1] + c2 * p[2] + c3;
    op[i] = r;
    if (normals) {
      f32x4 nv = skin_nrm_in(nrm + i);
      on[i] = skin_nrm_out(skin_unit(c0 * nv[0] + c1 * nv[1] + c2 * nv[2]));
    }
  }
}
/* Vertices first .. first + n - 1 of the cluster or strip streams, of
   palette pal (skin_vertices: n1, n2). */
static void skin_run(u32 first, u32 n1, u32 n2, u32 n, u32 pal, int strips, int normals) {
  int o = strips ? MXM_SPOS - MXM_POS : 0;
  const s16 *pos = (const s16 *)sec(MXM_RPOS + o) + 3 * first;
  const s8x4 *nrm = (const s8x4 *)sec(MXM_RNRM + o) + first;
  const u32 *jnt = (const u32 *)sec(MXM_JNT + o) + first;
  const u8x4 *wgt = (const u8x4 *)sec(MXM_WGT + o) + first;
  const f32x4(*pm)[4] = pal_joints(pal);
  if (normals)
    skin_vertices(pos, nrm, jnt, wgt, pm, skin_pos[skin_buf] + first, skin_nrm[skin_buf] + first,
                  n1, n2, n, 1);
  else
    skin_vertices(pos, nrm, jnt, wgt, pm, skin_pos[skin_buf] + first, skin_nrm[skin_buf] + first,
                  n1, n2, n, 0);
}

/* This frame's skinning (after mx_frame_begin and joints_frame), of the
   layout drawn. */
static void skin_frame(void) {
  st_frame.skin = st_frame.skinned = 0;
  if (!M.skinned || V.shader == S_SKIN)
    return;
  /* frame n - 2, which used this buffer, is done */
  while (mx_frame_done() + 1 < mx_frame_number())
    ;
  skin_buf = (int)(mx_frame_number() & 1);
  int rest = !animated(V.shader), layout = 1 + ix_now();
  if (rest && skin_rest[skin_buf] == layout)
    return;
  skin_rest[skin_buf] = rest ? layout : 0;
  u32 t0 = mx_count(), n;
  int normals = rest || V.shader != S_FLAT;
  n = 0;
  if (ix_now()) {
    for (u32 gi = 0; gi < ngrp[1]; gi++) {
      const struct group *q = &grp[1][gi];
      if (rest || (lod_need[q->mat & 0xFF] >> (q->mat >> 8) & 1)) {
        for (u32 c = q->c; c < q->e; c++)
          skin_run(M.cl[c].first, cl_n1[c], cl_n2[c], M.cl[c].nv, q->pal, 0, normals);
        n += q->nv;
      }
    }
    mx_dcache_writeback(skin_pos[skin_buf], M.h->nvert * 16);
    if (normals)
      mx_dcache_writeback(skin_nrm[skin_buf], M.h->nvert * 4);
  } else {
    for (u32 r = 0; r < M.h->nstrip; r++)
      if (rest || (lod_need[M.strip[r].mat & 0xFF] >> (M.strip[r].mat >> 8) & 1)) {
        skin_run(M.strip[r].first, 0, 0, M.strip[r].n, M.strip[r].pal, 1, normals);
        n += M.strip[r].n;
      }
    mx_dcache_writeback(skin_pos[skin_buf], M.h->nsvert * 16);
    if (normals)
      mx_dcache_writeback(skin_nrm[skin_buf], M.h->nsvert * 4);
  }
  st_frame.skin = mx_count() - t0, st_frame.skinned = n;
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
  int sk = M.skinned; /* skin_frame's */
  st[k++] = sk ? (const void *)skin_pos[skin_buf] : sec(MXM_POS + o);
  if (shader >= S_LIT)
    st[k++] = sk ? (const void *)skin_nrm[skin_buf] : sec(MXM_NRM + o);
  if (shader >= S_TEX)
    st[k++] = sec(MXM_UV + o);
}

/* Whether all of cluster c is in front of the view and inside the guard band
   (mvp as mvp_of makes it; mats: the joint matrices of the skinned
   programs, else 0). Its sphere moves with its joint, made bigger: its other
   joints move its vertices a little differently. */
/* The level of each material of a copy (mvp as mvp_of makes it) into sel,
   and into lod_need. */
static void lod_choose(const float mvp[4][4], u8 *sel) {
  float k = R / M.h->radius;
  for (u32 mt = 0; mt < M.h->nmat; mt++) {
    int l = lod_force;
    if (l < 0) {
      const float *c = part[mt];
      float depth = mvp[0][3] * c[0] + mvp[1][3] * c[1] + mvp[2][3] * c[2] + mvp[3][3];
      float r = c[3] * k, rpx = depth > r ? r * FY / depth : lod_r0;
      l = 0;
      for (float lim = lod_r0 * 0.5f; rpx < lim && l < 15; lim *= 0.5f)
        l++;
    }
    sel[mt] = (u8)(l < (int)nlod ? l : (int)nlod - 1);
    lod_need[mt] |= (u16)(1u << sel[mt]);
  }
}

static int cluster_inside(u32 c, const float mvp[4][4], const float (*mats)[4]) {
  const float *sp = M.cl[c].sphere;
  float p[3] = {sp[0], sp[1], sp[2]}, r = sp[3];
  if (mats && cl_joint[c] != 0xFFFF) {
    const float(*j)[4] = mats + 4 * cl_joint[c];
    for (int k = 0; k < 3; k++)
      p[k] = j[0][k] * sp[0] + j[1][k] * sp[1] + j[2][k] * sp[2] + j[3][k];
    r = r * 1.5f + 0.03f * M.h->radius;
  }
  float q[4];
  for (int k = 0; k < 4; k++)
    q[k] = mvp[0][k] * p[0] + mvp[1][k] * p[1] + mvp[2][k] * p[2] + mvp[3][k];
  /* view space: q.w the depth; q.x / q.w = 2048 + FX x / depth, so for y */
  float rw = r * R / M.h->radius, z = q[3] - rw;
  float x = (q[0] - 2048 * q[3]) / FX, y = (q[1] - 2048 * q[3]) / FY;
  x = x < 0 ? -x : x, y = y < 0 ? -y : y;
  return z > NEAR_Z && (x + rw) * FX < GUARD * z && (y + rw) * FY < GUARD * z;
}

/* The uniforms of mscei and msceic (in uni after the MVP): the MVP without
   the screen offset (it adds it: prm), and its z of the guard band's size:
   t.z = Z_K (ZK / depth - ZK / 2), GUARD at depth 1 (NEAR_Z), -GUARD far
   away; the GS's z is t.z / Z_K + ZK / 2 (scl, prm). The key light (light,
   model space, as the other shaders), a fill from the other side and a rim
   light from behind, each modulating the texture (128 = 1.0). */
#define Z_K (GUARD / (ZK / 2))
static void scei_uniforms(const float mvp[4][4], const float light[3]) {
  for (int j = 0; j < 4; j++) {
    uni[j][0] = mvp[j][0] - 2048 * mvp[j][3];
    uni[j][1] = mvp[j][1] - 2048 * mvp[j][3];
    uni[j][2] = -Z_K * (ZK / 2) * mvp[j][3] + (j == 3 ? Z_K * ZK : 0);
  }
  static const float fill_w[3] = {-0.62f, 0.25f, -0.74f}, rim_w[3] = {0.0f, -0.42f, -0.91f};
  float fill[3], rim[3];
  turn_back(fill_w, fill);
  turn_back(rim_w, rim);
  const float *dir[3] = {light, fill, rim};
  static const float strength[3] = {0.72f, 0.30f, 0.22f};
  for (int k = 0; k < 3; k++) {
    for (int i = 0; i < 3; i++) {
      uni[VU_IX_MSCEI_A + k][i] = dir[i][k] / 127.0f;
      uni[VU_IX_MSCEI_E + i][k] = 128.0f * strength[i];
    }
    uni[VU_IX_MSCEI_A + k][3] = uni[VU_IX_MSCEI_E + k][3] = 0;
    uni[VU_IX_MSCEI_AMB][k] = 128.0f * 0.26f;
    uni[VU_IX_MSCEI_SCL][k] = k == 2 ? 1 / Z_K : 1;
    uni[VU_IX_MSCEI_PRM][k] = k == 2 ? ZK / 2 : 2048;
  }
  uni[VU_IX_MSCEI_AMB][3] = 0; /* no highlight (TFX HIGHLIGHT2 adds the alpha) */
  uni[VU_IX_MSCEI_PRM][3] = GUARD;
  uni[VU_IX_MSCEI_SCL][3] = 0;
}

/* The uniforms of shader for material m, palette p, frame of the
   animation. */
static void set_uniforms(int shader, const float mvp[4][4], const float light[3],
                         const float eye[3], u32 m, u32 p, u32 frame) {
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
  if (shader == S_SCEI || shader == S_SCEIC) {
    scei_uniforms(mvp, light);
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
  /* the highlight: the material's strength (mxm_mat color[3]) */
  for (int k = 0; k < 3; k++)
    uni[VU_IX_MLIT_EYE][k] = eye[k];
  uni[VU_IX_MLIT_EYE][3] = (float)M.mat[m].color[3];
  if (shader != S_SKIN)
    return;
  (void)frame;
  const struct mxm_pal *pl = &M.pal[p];
  for (u32 j = 0; j < pl->n; j++)
    for (int c = 0; c < 4; c++)
      for (int k = 0; k < 4; k++)
        uni[VU_IX_MSKIN_JOINT + 4 * j + c][k] = jq[pl->joint[j]][c][k];
}

_Static_assert(VU_IX_MLIT_LIGHT == VU_IX_MTEX_LIGHT && VU_IX_MLIT_LIGHT == VU_IX_MSKIN_LIGHT &&
                   VU_IX_MLIT_LCOL == VU_IX_MSKIN_LCOL && VU_IX_MLIT_AMB == VU_IX_MSKIN_AMB &&
                   VU_IX_MLIT_EYE == VU_IX_MTEX_EYE && VU_IX_MLIT_EYE == VU_IX_MSKIN_EYE,
               "the lit shaders share the uniforms' places");

static void draw_copy(float ox, float oz, u32 frame, const u8 *lod) {
  int shader = V.shader, tex = shader >= S_TEX;
  float m[4][4], light[3], eye[3];
  mvp_of(m, ox, oz, light, eye);
  const struct mx_shader *s = &(M.skinned && shader != S_SKIN ? sh_ee : sh)[shader][ix_now()];
  /* AA1 only where nothing is blended */
  u32 prim_solid = (u32)mx_gs_prim(MX_GS_PRIM_TRISTRIP, 1, tex, 0, aa1, aa1, 0, 0, 0);
  u32 prim_blend = (u32)mx_gs_prim(MX_GS_PRIM_TRISTRIP, 1, tex, 0, 1, 0, 0, 0, 0);
  int npass = tex ? P_N : 1;
  const void *st[6];
  if (ix_now()) {
    streams_of(shader, 0, st);
    struct mx_ibatch *b = ibatch[s->out_qw == 3];
    /* the clusters drawn, when some may be outside the guard band */
    static struct mx_ibatch in[MAX_CLUSTER];
    /* flying, the model need not be all inside the guard band */
    int cull = fly;
    const float(*mats)[4] = animated(shader) ? anim_mats(frame) : 0;
    /* the clusters of one material and level (and palette: mskin), one draw */
    const struct group *g = grp[shader == S_SKIN];
    for (int pass = 0; pass < npass; pass++)
      for (u32 gi = 0; gi < ngrp[shader == S_SKIN]; gi++) {
        const struct group *q = &g[gi];
        u32 mat = q->mat & 0xFFu, n = 0;
        if ((pass != P_SOLID) != (tex && blended(mat)) || q->mat >> 8 != lod[mat])
          continue;
        if (cull) {
          for (u32 k = q->c; k < q->e; k++)
            if (cluster_inside(k, m, mats)) {
              in[n++] = b[k];
              st_frame.verts += M.cl[k].nv, st_frame.indices += M.cl[k].ni;
              st_frame.tris += pass == P_EDGE ? 0 : cl_tris[k];
            }
        } else {
          n = q->e - q->c;
          st_frame.verts += q->nv, st_frame.indices += q->ni;
          st_frame.tris += pass == P_EDGE ? 0 : q->tris;
        }
        if (n) {
          if (tex && (mat | (u32)pass << 8) != cur_mat)
            gs_state(mat, pass);
          bind(s);
          set_uniforms(shader, m, light, eye, mat, q->pal, frame);
          mx_dcache_writeback(uni, sizeof uni);
          mx_frame_shader_idraw(s, pass ? prim_blend : prim_solid, uni, st, cull ? in : b + q->c,
                                n);
          st_frame.draws++;
        }
      }
  } else {
    streams_of(shader, 1, st);
    for (int pass = 0; pass < npass; pass++)
      for (u32 r = 0; r < M.h->nstrip; r++) {
        const struct mxm_strip *sr = &M.strip[r];
        u32 mat = sr->mat & 0xFF;
        if ((pass != P_SOLID) != (tex && blended(mat)) || sr->mat >> 8 != lod[mat])
          continue;
        const void *at[6];
        for (u32 i = 0; i < s->nin; i++)
          at[i] = (const u8 *)st[i] + sr->first * s->in[i].bytes;
        if (tex && (mat | (u32)pass << 8) != cur_mat)
          gs_state(mat, pass);
        bind(s);
        set_uniforms(shader, m, light, eye, mat, sr->pal, frame);
        mx_dcache_writeback(uni, sizeof uni);
        mx_frame_shader_draw(s, pass ? prim_blend : prim_solid, uni, at, sr->n);
        st_frame.verts += strip_cost(s, sr->n), st_frame.indices += sr->n;
        st_frame.tris += pass == P_EDGE ? 0 : run_tris[r];
        st_frame.draws++;
      }
  }
}

static char hud[3][96];

/* One frame of the view at time t (seconds). */
static void frame(float t) {
  V.dist = clampf(V.dist, min_dist(), 20 * grid_radius());
  camera();
  st_frame.verts = st_frame.indices = st_frame.tris = st_frame.draws = 0;
  mx_frame_begin(36, 40, 52);
  {
    /* (Cs - Cd) As + Cd (blended passes, AA1), and the alpha test of the
       untextured programs (their alpha is 128) */
    u64 *c = mx_frame_direct(3);
    c = mx_e_ad_tag(c, 2, 1);
    c = mx_e_ad(c, MX_GS_REG_ALPHA_1, mx_gs_alpha(0, 1, 0, 1, 0));
    c = mx_e_ad(c, MX_GS_REG_TEST_1, mx_gs_test(1, 6, 0x40, 0, 0, 0, 1, 2));
    mx_frame_commit(c);
  }
  cur_mat = ~0u;
  u32 af = 0;
  if (animated(V.shader))
    af = (u32)(s32)(t * M.anim[V.anim].fps);
  phys_update(af, t);
  float step = 2.4f * R, o = -step * itof(V.copies - 1) * 0.5f;
  for (u32 mt = 0; mt < M.h->nmat; mt++)
    lod_need[mt] = 0;
  for (int z = 0; z < V.copies; z++)
    for (int x = 0; x < V.copies; x++) {
      float mvp[4][4], light[3], eye[3];
      mvp_of(mvp, o + step * itof(x), o + step * itof(z), light, eye);
      lod_choose(mvp, lod_sel[z * V.copies + x]);
    }
  if (M.skinned)
    joints_frame(animated(V.shader) ? (u32)V.anim : 0, af);
  skin_frame();
  for (int z = 0; z < V.copies; z++)
    for (int x = 0; x < V.copies; x++)
      draw_copy(o + step * itof(x), o + step * itof(z), af, lod_sel[z * V.copies + x]);
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
  p = hud_cat(p, ix_now() ? "  INDEXED  " : "  STRIPS  ");
  p = hud_u(p, (u32)(V.copies * V.copies));
  p = hud_cat(p, V.copies > 1 ? " COPIES" : " COPY");
  if (P.active)
    p = hud_cat(p, "  PHYS");
  if (flyby)
    p = hud_cat(p, "  FLYBY");
  if (aa1)
    p = hud_cat(p, "  AA1");
  if (mx_gs.smooth)
    p = hud_cat(p, "  SOFT");
  if (nlod > 1) {
    /* the level of each material (of the first copy) */
    p = hud_cat(p, "  LOD ");
    for (u32 mt = 0; mt < M.h->nmat; mt++)
      *p++ = (char)('0' + lod_sel[0][mt]), *p = 0;
  }
  if (animated(V.shader)) {
    p = hud_cat(p, V.shader == S_SKIN ? "  " : "  EE SKIN  ");
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
  hud_cat(p, ix_now() ? " INDICES" : " STRIP");
}

/* The busy shares of the sampler (since mx_trace_sample was started). */
static void busy(u32 *vu1, u32 *path, u32 *gs) {
  u32 b[MX_TS_N], n = mx_trace_busy(b);
  *vu1 = n ? b[6] * 100 / n : 0, *path = n ? b[7] * 100 / n : 0, *gs = n ? b[8] * 100 / n : 0;
}

struct result {
  u32 shader, indexed, frames, tris, verts, indices, vu1, path, gs, ee;
  u64 cycles; /* a slow run is longer than the 32-bit Count's 14.6 s */
  u32 skin, skinned; /* of the last frame */
};
static struct result results[2 * S_N];
static u32 nresults;

static void report(const struct result *r) {
  u32 fps10 = (u32)((u64)r->frames * MX_EE_HZ * 10 / r->cycles);
  u64 per_s = (u64)MX_EE_HZ * r->frames / r->cycles;
  mx_printf("model bench %-6s %-7s: %u.%u fps, %u triangles/s, %u VU1 vertices/s, %u %s/s; "
            "VU1 %u%% PATH %u%% GS %u%% EE %u%%\n",
            shader_names[r->shader], r->indexed ? "indexed" : "strips", fps10 / 10, fps10 % 10,
            (u32)(r->tris * per_s), (u32)(r->verts * per_s), (u32)(r->indices * per_s),
            r->indexed ? "indices" : "strip vertices", r->vu1, r->path, r->gs, r->ee);
  if (r->skinned)
    mx_printf("model bench %-6s %-7s: EE skin %u.%u ms a frame, %u vertices, %u cycles a vertex\n",
              shader_names[r->shader], r->indexed ? "indexed" : "strips",
              (u32)((u64)r->skin * 10000 / MX_EE_HZ / 10), (u32)((u64)r->skin * 10000 / MX_EE_HZ % 10),
              r->skinned, r->skin / r->skinned);
}

static int want_interactive;

/* frames of the view as it is, without vsync: the counts a second */
static void bench(int shader, int indexed) {
  V.shader = shader, V.indexed = indexed;
  mx_frame_vsync(0);
  u32 waits0 = mx_frame_stats.wait_buffer + mx_frame_stats.wait_ring;
  mx_trace_sample(1000);
  u32 last = mx_count(), f0 = mx_gs.frames, v0 = mx_gs.vsync;
  u64 cycles = 0;
  for (u32 f = 0; f < BENCH_FRAMES; f++) {
    if (pad_pressed() & MX_PAD_START)
      want_interactive = 1;
    frame(itof((s32)(mx_gs.vsync - v0)) / 60);
    if (f == 0) {
      hud_mode(hud[0]);
      hud_counts(hud[1]);
    }
    u32 now = mx_count();
    cycles += now - last, last = now;
  }
  u32 d0 = mx_count();
  while (mx_gs.frames_done < f0 + BENCH_FRAMES && mx_count() - d0 < 300000000u)
    ;
  struct result *r = &results[nresults++];
  r->shader = (u32)shader, r->indexed = (u32)indexed, r->frames = BENCH_FRAMES;
  r->cycles = cycles + (mx_count() - last);
  r->tris = st_frame.tris, r->verts = st_frame.verts, r->indices = st_frame.indices;
  r->skin = st_frame.skin, r->skinned = st_frame.skinned;
  busy(&r->vu1, &r->path, &r->gs);
  mx_trace_sample(0);
  u32 w = mx_frame_stats.wait_buffer + mx_frame_stats.wait_ring - waits0;
  r->ee = w < r->cycles ? (u32)((r->cycles - w) * 100 / r->cycles) : 0;
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

/* The layouts of the model (gltf2mxm.py --layout): strips, indexed. */
static int has_layout(int indexed) { return indexed ? M.h->ncluster != 0 : M.h->nstrip != 0; }

/* The view as it starts: around the model, from about 3 times the nearest
   distance that shows all of it; flying (indexed) from there. */
static void view_reset(void) {
  V.yaw = 0.6f, V.pitch = 0.25f, V.dist = 3.2f * min_dist();
  fly = 0;
  camera();
  fly_c[0] = cam_c[0], fly_c[1] = cam_c[1], fly_c[2] = cam_c[2];
  fly = V.indexed;
  if (fly) {
    /* walking: at the height of the model's eyes, looking at its middle */
    fly_c[1] = EYE_Y;
    V.pitch = EYE_Y / V.dist;
  }
}

/* One still frame of each shader as strips and indexed: the same picture. */
static int same_pictures(int nshader) {
  int fail = 0;
  if (!has_layout(0) || !has_layout(1)) {
    mx_printf("model: only %s, no pictures to compare\n", has_layout(1) ? "indexed" : "strips");
    return 0;
  }
  mx_frame_vsync(1);
  V.copies = copies(2), V.spin = 0.7f, V.dist = min_dist();
  _Static_assert(S_SCEIC == S_SCEI + 1, "msceic is compared with the mscei strips before it");
  for (int s = 0; s < nshader; s++) {
    /* msceic: its strips (in line_hash[1]) against mscei's (line_hash[0]) */
    for (int ix = !has_ix(s); ix < 2; ix++) {
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
    const char *other = has_ix(s) ? "indexed" : shader_names[S_SCEI];
    if (diff) {
      mx_printf("model: %s strips and %s differ in %u lines (the first %u)\n", shader_names[s],
                other, diff, first);
      fail = 1;
    } else {
      mx_printf("model: %s strips and %s: the same picture\n", shader_names[s], other);
    }
  }
  return fail;
}

/* Model i of models: opened (its textures uploaded, the HUD's after them),
   the view made right for it. Returns the number of its shaders, or -1. */
static int use_model(u32 i) {
  const struct mx_shader *ix[S_N];
  u32 nix = 0;
  for (int s = 0; s < S_N; s++)
    if (has_ix(s))
      ix[nix++] = &sh[s][1];
  if (model_open(models[i].base, models[i].size, ix, nix) < 0)
    return -1;
  M.name = models[i].name, cur_model = i;
  int skinned = (M.h->flags & MXM_SKINNED) && M.h->nanim && M.h->npal;
  M.skinned = skinned;
  /* the EE skins it for the programs but mskin (it has no rest pose
     streams): the bind pose of both layouts there, room for it, the joint
     matrices of the animations aligned for LQC2 */
  M.ee_skin = skinned && M.h->nvert <= MAX_SKIN && M.h->nsvert <= MAX_SKIN;
  for (int s = MXM_RPOS; s <= MXM_WGT; s++)
    M.ee_skin &= !M.h->nvert || M.h->off[s];
  for (int s = MXM_SRPOS; s <= MXM_SWGT; s++)
    M.ee_skin &= !M.h->nsvert || M.h->off[s];
  for (u32 a = 0; a < M.h->nanim; a++)
    M.ee_skin &= M.anim[a].mat % 16 == 0;
  if (skinned && !M.ee_skin) {
    mx_printf("model: the EE cannot skin it (%u, %u vertices)\n", M.h->nvert, M.h->nsvert);
    return -1;
  }
  skin_rest[0] = skin_rest[1] = 0;
  mx_printf("model: %s: %u triangles, %u clusters, %u vertices in clusters, %u in strips, %u "
            "materials, %u joints, %u animations\n",
            M.name, M.h->ntri, M.h->ncluster, M.h->nvert, M.h->nsvert, M.h->nmat, M.h->njoint,
            M.h->nanim);
  V.anim = 0;
  for (u32 a = 0; a < M.h->nanim; a++)
    if (M.anim[a].name[0] == 'W' && M.anim[a].name[1] == 'a') /* Walk, if there is one */
      V.anim = (int)a;
  if (skinned && !V.anim && M.h->nanim > 1)
    V.anim = 1;
  if (!has_layout(V.indexed))
    V.indexed ^= 1;
  if (!skinned && V.shader == S_SKIN)
    V.shader = S_TEX;
  V.copies = copies(V.copies);
  phys_open();
  hud_init(M.tbp_free, SH);
  return skinned ? S_N : S_SKIN;
}

static void interactive(int nshader) {
  mx_printf("model: interactive (SELECT + START ends)\n");
  mx_frame_vsync(V.vsync = 1);
  V.copies = 1, V.turning = 0, V.shader = S_SCEI;
  view_reset();
  flyby = 1;
  u32 v0 = mx_gs.vsync, n = 0, tp = mx_count(), fp = mx_gs.frames, fv = v0;
  float yaw0 = V.yaw;
  u64 tri = 0, vert = 0;
  mx_trace_sample(1000);
  for (;;) {
    u32 b = pad_pressed(), held = mx_pad_buttons(0);
    if ((held & (MX_PAD_SELECT | MX_PAD_START)) == (MX_PAD_SELECT | MX_PAD_START))
      break;
    if (b & MX_PAD_SQUARE) {
      V.shader = (V.shader + 1) % nshader;
      if (V.shader == S_SCEIC && !has_layout(0)) /* it draws strips */
        V.shader = (V.shader + 1) % nshader;
    }
    if ((b & MX_PAD_CROSS) && has_layout(!V.indexed)) {
      V.indexed ^= 1;
      view_reset(); /* strips: around the model */
    }
    if ((b & MX_PAD_SELECT) && !(held & MX_PAD_START) && nmodel > 1) {
      /* the textures change: once the frames before are drawn */
      u32 last = mx_gs.frames - 1, d0 = mx_count();
      while (mx_gs.frames_done < last + 1 && mx_count() - d0 < 300000000u)
        ;
      mx_frame_stop();
      pipeline_restart();
      mx_frame_vsync(V.vsync);
      u32 was = cur_model;
      int ns = use_model((cur_model + 1) % nmodel);
      if (ns < 0)
        ns = use_model(was);
      nshader = ns;
      /* the view stays, unless it can no longer walk (strips only) */
      if (fly != V.indexed)
        view_reset();
    }
    if ((b & MX_PAD_TRIANGLE) && (M.h->flags & MXM_SINGLE)) {
      phys_off ^= 1;
      P.reset = 1;
    } else if (b & MX_PAD_TRIANGLE) {
      V.copies = V.copies >= 16 ? 1 : copies(V.copies * 2);
      view_reset();
    }
    if ((b & MX_PAD_CIRCLE) && M.h->nanim) {
      V.anim = (V.anim + 1) % (int)M.h->nanim;
      P.reset = 1;
    }
    if (b & MX_PAD_SQUARE)
      P.reset = 1;
    if (b & MX_PAD_L2)
      V.turning ^= 1;
    if (b & MX_PAD_R2)
      mx_frame_vsync(V.vsync ^= 1);
    if (b & MX_PAD_L3) {
      view_reset();
      flyby = 1, fv = mx_gs.vsync, yaw0 = V.yaw;
    }
    if (b & MX_PAD_R3) {
      int m = (aa1 | mx_gs.smooth << 1) + 1;
      aa1 = m & 1;
      mx_gs_smooth(m >> 1 & 1);
    }
    u8 sticks[4];
    mx_pad_sticks(0, sticks);
    float lx = itof((s32)sticks[2] - 128) / 128, ly = itof((s32)sticks[3] - 128) / 128;
    float rx = itof((s32)sticks[0] - 128) / 128, ry = itof((s32)sticks[1] - 128) / 128;
    lx = lx * lx < 0.02f ? 0 : lx, ly = ly * ly < 0.02f ? 0 : ly;
    rx = rx * rx < 0.02f ? 0 : rx, ry = ry * ry < 0.02f ? 0 : ry;
    /* the D-pad (a digital pad): turn left and right, walk forwards and back */
    if (held & MX_PAD_LEFT)
      rx = -1;
    if (held & MX_PAD_RIGHT)
      rx = 1;
    if (held & MX_PAD_UP)
      ly = -1;
    if (held & MX_PAD_DOWN)
      ly = 1;
    if (flyby && (lx != 0 || ly != 0 || rx != 0 || ry != 0 || (held & (MX_PAD_R1 | MX_PAD_L1)))) {
      /* the pad takes over, from where the camera is (indexed: walking) */
      flyby = 0;
      fly = V.indexed;
      fly_c[0] = cam_c[0], fly_c[1] = cam_c[1], fly_c[2] = cam_c[2];
    }
    if (flyby) {
      /* around in 20 s; up and down, near and far, slower and out of step */
      float ft = itof((s32)(mx_gs.vsync - fv)) / 60;
      V.yaw = yaw0 + ft * 0.314f;
      V.pitch = 0.2f + 0.2f * sinf_(ft * 0.23f);
      V.dist = min_dist() * (1.6f + 0.6f * sinf_(ft * 0.17f));
      fly = 0;
    }
    /* as Fortnite: the right stick looks (up looks up), the left one walks
       (forwards and back, sideways: on the ground, at the eye's height) */
    V.yaw -= rx * 0.045f;
    V.pitch = clampf(V.pitch + ry * 0.03f, -1.4f, 1.4f);
    camera();
    if (fly) {
      float sy = sinf_(V.yaw), cy = cosf_(V.yaw);
      fly_c[0] += WALK * (lx * cy + ly * sy);
      fly_c[2] += WALK * (-lx * sy + ly * cy);
      /* R1 up, L1 down */
      fly_c[1] += WALK * (itof(!!(held & MX_PAD_R1)) - itof(!!(held & MX_PAD_L1)));
    }
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
static void record(void) {
  mx_frame_vsync(1);
  V.copies = copies(MODEL_RECORD_COPIES), V.indexed = has_layout(1);
  V.shader = S_SCEI;
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
  sh[S_SCEI][0] = MX_VU_SHADER(MSCEI, mscei_vu, mscei_vu_end);
  sh[S_SCEI][1] = MX_VU_ISHADER(IX_MSCEI, mscei_ix_vu, mscei_ix_vu_end);
  sh[S_SCEIC][0] = MX_VU_SHADER(MSCEIC, msceic_vu, msceic_vu_end);
  sh[S_SKIN][0] = MX_VU_SHADER(MSKIN, mskin_vu, mskin_vu_end);
  sh[S_SKIN][1] = MX_VU_ISHADER(IX_MSKIN, mskin_ix_vu, mskin_ix_vu_end);
  for (int s = 0; s < S_N; s++)
    for (int k = 0; k < 2; k++) {
      sh_ee[s][k] = sh[s][k];
      sh_ee[s][k].in[0].fmt = MX_UNPACK_V4_32, sh_ee[s][k].in[0].bytes = 16;
    }

  /* the models: from the host, else the one built in */
  u32 used = 0;
  for (int a = 1; a < argc && nmodel < MAX_MODELS; a++) {
    if (!strcmp_(argv[a], "-i")) {
      want_interactive = 1;
      continue;
    }
    if (!strcmp_(argv[a], "-nophys")) {
      phys_off = 1;
      continue;
    }
    if (!strcmp_(argv[a], "-lod") && a + 1 < argc) {
      lod_force = (int)atou_(argv[++a]);
      continue;
    }
    if (!strcmp_(argv[a], "-lodr") && a + 1 < argc) {
      lod_r0 = itof((s32)atou_(argv[++a]));
      continue;
    }
    if (!strcmp_(argv[a], "-static")) {
      static_pose = 1;
      continue;
    }
    if (!strcmp_(argv[a], "-cutout")) {
      cutout = 1;
      continue;
    }
    if (!strcmp_(argv[a], "-aa")) {
      aa1 = 1;
      continue;
    }
    if (!strcmp_(argv[a], "-soft")) {
      mx_gs_smooth(1);
      continue;
    }
    int n = model_read(argv[a], file_buf + used, MAX_FILE - used);
    if (n > 0) {
      model_add(file_buf + used, (u32)n, argv[a]);
      used += ((u32)n + 63) & ~63u;
      mx_printf("model: %s, %d bytes\n", argv[a], n);
    } else {
      mx_printf("model: %s could not be read\n", argv[a]);
    }
  }
  if (!nmodel)
    model_add(model_fox, (u32)(model_fox_end - model_fox), "fox");
  int nshader = use_model(0);
  if (nshader < 0) {
    MX_TEST_END(1);
    return 1;
  }
  if (mx_pad_init() < 0)
    mx_printf("model: no pad driver\n");
#ifdef MODEL_RECORD
  record();
  mx_frame_stop();
  MX_TEST_END(0);
  return 0;
#endif

  int fail = want_interactive ? 0 : same_pictures(nshader);

  /* the counts a second of each shader, both ways */
  V.copies = copies(BENCH_COPIES), V.spin = 0.3f, V.dist = 1.15f * min_dist();
  for (int s = 0; s < nshader && !want_interactive; s++)
    for (int i = 0; i < 2 && !want_interactive; i++)
      if (has_layout(i) && (!i || has_ix(s)))
        bench(s, i);
  if (!want_interactive) {
    mx_printf("model bench: %u x %u copies of %u triangles\n", (u32)V.copies, (u32)V.copies,
              M.h->ntri);
    for (u32 i = 0; i + 1 < nresults; i++)
      if (results[i].shader == results[i + 1].shader)
        mx_printf("model bench %-6s: indexed / strips: %u%% of the time, %u%% of the VU1 "
                  "vertices\n",
                  shader_names[results[i].shader],
                  results[i].cycles ? (u32)(results[i + 1].cycles * 100 / results[i].cycles)
                                    : 0,
                  results[i].verts ? results[i + 1].verts * 100 / results[i].verts : 0);
    /* the strips of msceic and mscei: what the clipping costs */
    const struct result *nc = 0, *cl = 0;
    for (u32 i = 0; i < nresults; i++)
      if (!results[i].indexed && results[i].shader == S_SCEI)
        nc = &results[i];
      else if (!results[i].indexed && results[i].shader == S_SCEIC)
        cl = &results[i];
    if (nc && cl && nc->cycles)
      mx_printf("model bench %-6s: clipped / not (%s strips): %u%% of the time\n",
                shader_names[S_SCEIC], shader_names[S_SCEI],
                (u32)(cl->cycles * 100 / nc->cycles));
    /* a picture of the view */
    mx_frame_vsync(1);
    V.copies = 1, V.shader = S_SCEI, V.indexed = has_layout(1), V.dist = 0;
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
