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

   A skinned model animates with every program: the EE and VU0 skin the
   vertices into the streams they read: the
   clusters by VU0 micro programs fed through VF registers (skin_clusters),
   the strips (and all with -skin macro) in VU0 macro mode; a pose at least
   every k frames (-pose <k>, 2), more often when the EE has the time, in its
   waits for VU1 (pose_frame); -pose 1: a pose each frame, before it draws.
   -static: they draw the rest pose. At the start all clusters are skinned
   both ways once, which must give the same (skin_check).

   A model with levels of detail (fbx2glb.py --lods) draws each part at the
   level its projected size asks for: level 0 down to a radius of R0 / 2
   pixels, one level more at each halving (-lodr <px>: R0, 512 by default;
   -lod <k>: always level k).

   The models: host:<argv[1]> .. (.mxm files, from ps2client's directory),
   else the one built in (Fox, demos/model/assets). The run is of the first;
   SELECT in the interactive mode goes to the next. A model of outfits
   (MXM_OUTFIT: fbx2glb.py with an FBX for each) draws one of them (-outfit
   <k>: k, else the first); SELECT goes to its next, after the last to the
   next model. -i (before or between
   them): the interactive mode at once; -aa: AA1 (the GS draws the edges of
   the triangles with their coverage); -soft, -deflicker: the display filter
   (mx_gs_smooth: half a pixel across, or a line down, blended); -ss [n]:
   supersampling, n samples a pixel (4, 8, 16, 32; ss_fit); -nocull: each
   tile draws all clusters, not only those that reach it. R3: the next of
   quality[] (AA1, the filters, supersampling). -mip off|on|tri|col,
   -mipbias <n>: MIPMAP (mip_mode, by default on). -cutout: the
   materials with alpha only cut out (alpha test), not blended. -nophys: no
   springs (MXM_PHYS: joints such as hair and cloth pulled towards the
   animation by springs, live, in world space). -mfifo: the frame stream
   through the MFIFO, every copy sending all its batches; by default the
   stream is in chain mode (mx_frame_init_chain) and each draw's batches are
   built once a frame as a mesh chain that every copy calls.

   The run, without a pad: for each shader one frame as strips and one
   indexed, which must be the same picture (the outputs are the same, bit
   for bit; msceic: the same as mscei, nothing is beyond the guard band);
   then each shader both ways without vsync for BENCH_FRAMES frames,
   BENCH_COPIES x BENCH_COPIES copies (frames, triangles, vertices and
   indices a second, the units' busy shares); then a screenshot
   (host:model.tga). START on pad 0 during the run: the interactive mode.

   It starts with mscei, the model animated and turning slowly, the view on
   its upper half:

     left stick          zoom                      right stick         turn the model,
     D-pad               turn, zoom                                    tilt the view
     L3                  the view as it started
     SQUARE              next shader               CROSS              strips / indexed
     TRIANGLE            copies 1, 2x2 .. 16x16 (a single model: springs on/off)
     CIRCLE              next animation
     L2                  turning on/off            R2                 vsync on/off (off)
     SELECT              next outfit / model       R3                 AA1 / filters / SS 4, 16
     L1                  MIPMAP: on / trilinear / level colours / off
     R1                  MIPMAP bias: -1, -1/2, 0, +1/2, +1 level
     SELECT + START      end

   The view is never nearer than the whole model fits the guard band (the
   programs do not clip). Turning the model stops its turning by itself; L2
   turns that on and off.

   -DMODEL_RECORD=<frames>: instead, that many frames of the model walking
   (or turning), MODEL_RECORD_COPIES x MODEL_RECORD_COPIES copies (1), 30 a
   second whatever the speed, each saved as host:rec_<n>.tga
   (tools/model/frames2video.py makes a video of them). -DMODEL_SHOTS:
   instead, still pictures and frame times of AA1 and supersampling
   (shots). */

#include <mx/mx.h>
#include <mx/pad.h>
#include <mx/sif.h>
#include <mx/tex.h>
#include "hud.h"
#include "mflat.h"
#include "mflat_ix.h"
#include "mlit.h"
#include "mlit_ix.h"
#include "mscei.h"
#include "mscei_ix.h"
#include "msceic.h"
#include "mtex.h"
#include "mtex_ix.h"
#include "mxm.h"

extern const u8 mflat_vu[], mflat_vu_end[], mlit_vu[], mlit_vu_end[], mtex_vu[], mtex_vu_end[],
    mscei_vu[], mscei_vu_end[], msceic_vu[], msceic_vu_end[], mflat_ix_vu[], mflat_ix_vu_end[],
    mlit_ix_vu[], mlit_ix_vu_end[], mtex_ix_vu[], mtex_ix_vu_end[], mscei_ix_vu[],
    mscei_ix_vu_end[];
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
_Static_assert(VU_MSCEI_A == VU_IX_MSCEI_A && VU_MSCEI_SCL == VU_IX_MSCEI_SCL &&
                   VU_MSCEIC_A == VU_IX_MSCEI_A && VU_MSCEIC_SCL == VU_IX_MSCEI_SCL,
               "");
#define JOINTS 24 /* of a palette (gltf2mxm.py --palette) */
/* mscei has the most uniforms */
_Static_assert(VU_IX_MSCEI_UNI_QW >= VU_IX_MFLAT_UNI_QW &&
                   VU_IX_MSCEI_UNI_QW >= VU_IX_MLIT_UNI_QW &&
                   VU_IX_MSCEI_UNI_QW >= VU_IX_MTEX_UNI_QW &&
                   VU_IX_MSCEI_UNI_QW >= VU_MSCEIC_UNI_QW,
               "");

/* the screen: 640 x 448 interlaced, every frame drawn in full */
#define SW 640
#define SH 448
static const float FX = 457, FY = 457 * 0.467f * SH / 224;
/* The GS's z is z_a / depth + z_b: the scene's sphere from Z_FAR (its far
   side) to Z_NEAR (its near side, or NEAR_Z), each frame (z_range); more is
   nearer. The programs' FTOI4 makes it 16 times that in XYZ2: 24-bit Z,
   up to 2^20; Z_MID its middle (msceic's guard band) */
#define Z_NEAR 1040000.0f
#define Z_FAR 8000.0f
#define Z_MID 524288.0f
static float z_a, z_b;
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

/* the room for the models from the host (-DMAX_FILE=64 when one is built in
   and big) */
#ifndef MAX_FILE
#define MAX_FILE (14u << 20)
#endif
#define MAX_CLUSTER 28672
#define MAX_MAT 16
#define MAX_PAL 448
/* GS memory in blocks (256 bytes), and the HUD's texture at the end of what
   the model's take: 256 x 8 CT32, 4 pages wide */
#define GS_BLOCKS (4u << 20 >> 8)
#define HUD_BLOCKS (4 * 32)
static u8 file_buf[MAX_FILE] MX_ALIGNED(64);
static u8 idx_buf[2][1u << 20] MX_ALIGNED(64); /* the indices times 2 and times 3 */
static struct mx_ibatch ibatch[2][MAX_CLUSTER];
/* of each cluster: its triangles (the strip without the joins) */
static u16 cl_tris[MAX_CLUSTER];
/* its first vertices of one joint (weight 255 in the first slot), the ones
   of two after them (gltf2mxm.py puts them first, in that order): the EE
   skins them in loops of their own */
static u16 cl_n1[MAX_CLUSTER], cl_n2[MAX_CLUSTER];
/* The draws of the clusters: the runs of one material, level and outfit
   mask ([1]: and palette, the EE's skinning), with their counts. */
#define MAX_GROUP 4096
static struct group {
  u32 c, e; /* clusters c .. e - 1 */
  u16 mat, pal;
  u32 nv, ni, tris;
  u32 mask; /* the outfits it is drawn in */
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
  struct mx_tex tex[MAX_MAT]; /* the textures with their levels and CLUTs (T8) */
  struct mx_tex probe;        /* MIP_COLOURS: a level a colour */
  u32 tbp_free;               /* the first block after the textures */
  const char *name;
  int skinned; /* animations, a palette */
  int ee_skin; /* and the EE can skin it */
  u32 noutfit; /* MXM_OUTFIT: the outfits, their names, the masks */
  const char (*outfit)[16];
  const u32 *cl_mask, *strip_mask;
} M;

static const void *sec(int s) { return M.h->off[s] ? M.base + M.h->off[s] : 0; }

/* The outfit drawn: the clusters and strip runs of the others are left out
   (all are drawn without MXM_OUTFIT). */
static u32 outfit;
static u32 cl_mask(u32 c) { return M.cl_mask ? M.cl_mask[c] : ~0u; }
static u32 run_mask(u32 r) { return M.strip_mask ? M.strip_mask[r] : ~0u; }
static int worn(u32 mask) { return mask >> outfit & 1; }

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
/* MIPMAP (L1; -mip off|on|tri|col): level 0 only (bilinear), the nearest
   level (bilinear), between the two nearest (trilinear), or a colour for
   each level instead of the texture (point sampled: which level is used
   where). The level comes from K = the material's texel density (mxm_mat
   lod) over the pixels a model unit at w = 1, plus mip_bias (R1; -mipbias
   <n>: n / 16 of a level, more is softer). */
enum { MIP_OFF, MIP_ON, MIP_TRI, MIP_COLOURS, MIP_N };
static const char *const mip_names[MIP_N] = {"", "  MIP", "  TRILINEAR", "  MIP COLOURS"};
static int mip_mode = MIP_ON;
static s32 mip_bias;

/* MIP_COLOURS: 64 x 64 .. 2 x 2, red, yellow, green, cyan, blue, magenta */
#define PROBE_LEVELS 6
static u32 probe_px[64 * 64 + 32 * 32 + 16 * 16 + 8 * 8 + 4 * 4 + 2 * 2] MX_ALIGNED(64);
static const void *const *probe_levels(void) {
  static const u32 col[PROBE_LEVELS] = {0x800000FF, 0x8000FFFF, 0x8000FF00,
                                        0x80FFFF00, 0x80FF0000, 0x80FF00FF};
  static const void *lv[PROBE_LEVELS];
  u32 *p = probe_px;
  for (u32 n = 0; n < PROBE_LEVELS; n++) {
    lv[n] = p;
    for (u32 i = 0; i < (64u >> n) * (64u >> n); i++)
      *p++ = col[n];
  }
  mx_dcache_writeback(probe_px, sizeof probe_px);
  return lv;
}

/* Material m's texture (its levels) into the GS. */
static void tex_upload(u32 m) {
  const struct mxm_mat *mt = &M.mat[m];
  const u8 *tex = sec(MXM_TEX);
  u32 t8 = mt->psm == MXM_TEX_T8, levels = mt->levels ? mt->levels : 1;
  /* the levels one after the other, each from 16 bytes */
  const void *lv[MX_TEX_LEVELS];
  u32 off = mt->tex;
  for (u32 n = 0; n < levels; n++) {
    lv[n] = tex + off;
    off += ((1u << (mt->tw - n)) * (1u << (mt->th - n)) * (t8 ? 1 : 4) + 15) & ~15u;
  }
  mx_frame_tex_upload(0, &M.tex[m], lv);
  mx_frame_wait_path3();
}

/* Where the level colours are: in the place of material probe_host's
   texture (the largest; uploaded while MIP_COLOURS is on, the texture again
   after it), else after the HUD (-2), else nowhere (-1: no MIP_COLOURS).
   The model's textures have all of the GS memory after the framebuffers
   but the CLUTs' and the HUD's. */
static int probe_host = -1;
static void probe_upload(void) {
  mx_frame_tex_upload(1, &M.probe, probe_levels());
  mx_frame_wait_path3();
}
static void probe_place(void) {
  u32 pb = mx_tex_init(&M.probe, 0, MX_GS_PSM_CT32, 6, 6, PROBE_LEVELS, MX_TEX_PACKED), hb = 0;
  probe_host = -1;
  for (u32 m = 0; m < M.h->nmat; m++)
    if (M.tex[m].blocks > hb)
      hb = M.tex[m].blocks, probe_host = (int)m;
  if (hb >= pb) {
    mx_tex_init(&M.probe, M.tex[probe_host].tbp[0], MX_GS_PSM_CT32, 6, 6, PROBE_LEVELS,
                MX_TEX_PACKED);
  } else if (M.tbp_free + HUD_BLOCKS + ((pb + 31) & ~31u) <= GS_BLOCKS) {
    mx_tex_init(&M.probe, M.tbp_free + HUD_BLOCKS, MX_GS_PSM_CT32, 6, 6, PROBE_LEVELS,
                MX_TEX_PACKED);
    probe_host = -2;
  } else {
    probe_host = -1;
  }
  if (probe_host == -1 && mip_mode == MIP_COLOURS)
    mip_mode = MIP_ON;
  if (probe_host == -2 || (probe_host >= 0 && mip_mode == MIP_COLOURS))
    probe_upload();
}

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
  const struct mxm_outfit *of = sec(MXM_OUTFIT);
  M.noutfit = 0, M.outfit = 0, M.cl_mask = M.strip_mask = 0;
  if (of) {
    u32 room = h->bytes - h->off[MXM_OUTFIT];
    if (!of->n || of->n > 32 || 16 + 16 * of->n > room || of->cl % 16 || of->strip % 16 ||
        of->cl + 4 * h->ncluster > room || of->strip + 4 * h->nstrip > room) {
      mx_printf("model: the outfits are not right\n");
      return -1;
    }
    M.noutfit = of->n, M.outfit = (const void *)(of + 1);
    M.cl_mask = (const void *)((const u8 *)of + of->cl);
    M.strip_mask = (const void *)((const u8 *)of + of->strip);
  }
  if (outfit >= (M.noutfit ? M.noutfit : 1))
    outfit = 0;
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
    cl_n1[c] = cl_n2[c] = 0;
    if (jnt && wgt && h->npal) {
      u32 v = k->first, e = k->first + k->nv;
      while (v < e && wgt[4 * v] == 255)
        v++;
      cl_n1[c] = (u16)(v - k->first);
      while (v < e && !wgt[4 * v + 2] && !wgt[4 * v + 3])
        v++;
      cl_n2[c] = (u16)(v - k->first - cl_n1[c]);
      for (u32 v = k->first; v < k->first + k->nv; v++)
        for (int i = 0; i < 4; i++)
          if (jnt[4 * v + i] >= M.pal[k->pal].n) {
            mx_printf("model: cluster %u: a joint not in its palette\n", c);
            return -1;
          }
    }
  }
  for (int by_pal = 0; by_pal < 2; by_pal++) {
    u32 n = 0;
    for (u32 c = 0; c < h->ncluster;) {
      u32 e = c + 1;
      while (e < h->ncluster && M.cl[e].mat == M.cl[c].mat && cl_mask(e) == cl_mask(c) &&
             (!by_pal || M.cl[e].pal == M.cl[c].pal))
        e++;
      if (n == MAX_GROUP) {
        mx_printf("model: more than %u draws\n", MAX_GROUP);
        return -1;
      }
      struct group *g = &grp[by_pal][n++];
      *g = (struct group){c, e, M.cl[c].mat, M.cl[c].pal, 0, 0, 0, cl_mask(c)};
      for (; c < e; c++)
        g->nv += M.cl[c].nv, g->ni += M.cl[c].ni, g->tris += cl_tris[c];
    }
    ngrp[by_pal] = n;
  }
  /* the levels; each material's sphere from its level 0 clusters (the
     model's without clusters) */
  if (h->nstrip > MAX_STRIP) {
    mx_printf("model: %u strip runs\n", h->nstrip);
    return -1;
  }
  for (u32 r = 0; r < h->nstrip; r++) {
    const struct mxm_strip *k = &M.strip[r];
    if (k->first % 4 || k->first + k->n > h->nsvert || (k->mat & 0xFF) >= h->nmat ||
        (k->pal != 0xFFFF && k->pal >= h->npal) || (k->pal == 0xFFFF && h->njoint > 256)) {
      mx_printf("model: strip run %u is not right\n", r);
      return -1;
    }
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
  for (u32 r = 0; r < h->nstrip; r++)
    run_tris[r] = M.strip[r].tris;
  /* an index is the vertex times the program's outputs (2 or 3) */
  for (u32 i = 0; i < idx_bytes; i++)
    idx_buf[0][i] = (u8)(idx[i] * 2), idx_buf[1][i] = (u8)(idx[i] * 3);
  mx_dcache_writeback(idx_buf, sizeof idx_buf);
  for (u32 c = 0; c < h->ncluster; c++)
    for (int o = 0; o < 2; o++)
      ibatch[o][c] = (struct mx_ibatch){M.cl[c].first, M.cl[c].nv, M.cl[c].ni,
                                        idx_buf[o] + M.cl[c].idx};
  /* the textures after the framebuffers, each with its MIPMAP levels from
     a page (mx_tex_init); then the CLUTs, 4 blocks each; then the HUD's
     (M.tbp_free); the level colours (MIP_COLOURS) in the largest texture's
     place (probe_place) */
  u32 tbp = mx_gs.tbp, nclut = 0;
  for (u32 m = 0; m < h->nmat; m++) {
    const struct mxm_mat *mt = &M.mat[m];
    struct mx_tex *t = &M.tex[m];
    u32 t8 = mt->psm == MXM_TEX_T8, levels = mt->levels ? mt->levels : 1;
    u32 b = mx_tex_init(t, tbp, t8 ? MX_GS_PSM_T8 : MX_GS_PSM_CT32, mt->tw, mt->th, levels,
                        MX_TEX_PACKED);
    if (tbp + ((b + 31) & ~31u) > GS_BLOCKS - HUD_BLOCKS) {
      mx_printf("model: the textures do not fit the GS memory\n");
      return -1;
    }
    tex_upload(m);
    if (t8)
      t->cbp = nclut++;
    tbp += (b + 31) & ~31u;
  }
  u32 clut_blocks = (4 * nclut + 31) / 32 * 32;
  if (tbp + clut_blocks > GS_BLOCKS - HUD_BLOCKS) {
    mx_printf("model: the textures do not fit the GS memory\n");
    return -1;
  }
  const u8 *tex = sec(MXM_TEX);
  for (u32 m = 0; m < h->nmat; m++)
    if (M.mat[m].psm == MXM_TEX_T8) {
      M.tex[m].cbp = tbp + 4 * M.tex[m].cbp;
      mx_frame_texture(0, M.tex[m].cbp, 1, MX_GS_PSM_CT32, 16, 16, tex + M.mat[m].clut);
      mx_frame_wait_path3();
    }
  tbp += clut_blocks;
  M.tbp_free = tbp;
  probe_place();
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

enum { S_FLAT, S_LIT, S_TEX, S_SCEI, S_SCEIC, S_N };
static const char *const shader_names[S_N] = {"MFLAT", "MLIT", "MTEX", "MSCEI", "MSCEIC"};
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
  float look; /* the height of the point the view circles, world units */
  int shader, indexed, copies, anim, turning, vsync;
} V = {0.6f, 0.25f, 0, 0, 0, S_SCEI, 1, 1, 0, 1, 1};

/* the view's shader draws indexed (msceic: strips, the view as indexed) */
static int ix_now(void) { return V.indexed && has_ix(V.shader); }

/* -static: the programs draw the rest pose (skinned once) */
static int static_pose;
/* shader draws the model animated (what the EE skinned) */
static int animated(int shader) {
  (void)shader;
  return M.skinned && !static_pose;
}

static float cam_c[3], cam_r[3], cam_u[3], cam_f[3];
/* Supersampling (R3, -ss <n>): the scene's box on the screen (ss_fit) is
   drawn ss_s times as large (2, less only near the guard band) into a
   512 x 256 target, a tile of the box at a time; a bilinear sprite draws
   each tile into the frame's buffer: at 2 each pixel the mean of 2 x 2.
   Each tile is drawn ss_j times, moved by less than a target pixel each
   time (ss_jitter), the sprites blended as a running mean: 4 ss_j samples a
   pixel. The target and its Z buffer are where mx_gs_init's Z buffer was.
   One projection for the whole box, its centre ss_c at the GS's ss_k (2048
   without): a tile only moves the target's XYOFFSET to it, and draws only
   the clusters whose spheres reach it (ss_bin; -nocull: all). */
#define SS_W 512
#define SS_H 256
#define SS_MAX_TILES 16
static int ss_j; /* passes a tile, 0: off */
static float ss_s = 1, ss_k[2] = {2048, 2048}, ss_c[2];
static u32 ss_box[4];   /* x0, y0, x1, y1: screen pixels */
static u32 ss_tiles[2]; /* across, down */
static struct mx_rt ss_rt;
static int ss_cull = 1;
static int ss_cur = -1; /* the tile being drawn, -1: none (the whole screen) */
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
/* The view turns around (0, V.look, 0) at V.dist, along yaw and pitch.
   The nearest depth of the scene, in world units: */
#define NEAR_Z 1.0f

/* n x n copies, or one of a model made to be drawn once (MXM_SINGLE). */
static int copies(int n) { return M.h->flags & MXM_SINGLE ? 1 : n; }

/* The radius of the copies together. */
static float grid_radius(void) {
  return R + 2.4f * R * itof(V.copies - 1) * 0.5f * 1.4143f;
}
static float min_dist(void) {
  float l = V.look < 0 ? -V.look : V.look;
  return (grid_radius() + l) * (1 + FX / GUARD);
}
/* The nearest distance that has all of it (its sphere) on the screen, but
   for 24 lines at the top and the bottom. */
static float fit_dist(void) {
  float t = (SH / 2 - 24) / FY; /* the tangent of half the view's height */
  return grid_radius() * __builtin_sqrtf(1 + 1 / (t * t));
}

/* z_a and z_b for this frame's camera: the depths of the scene's sphere
   (from NEAR_Z) over Z_FAR .. Z_NEAR. */
static void z_range(void) {
  float dc = -(cam_c[0] * cam_f[0] + cam_c[1] * cam_f[1] + cam_c[2] * cam_f[2]), r = grid_radius();
  float dn = dc - r > NEAR_Z ? dc - r : NEAR_Z, df = dc + r > dn + 1 ? dc + r : dn + 1;
  z_a = (Z_NEAR - Z_FAR) / (1 / dn - 1 / df), z_b = Z_NEAR - z_a / dn;
}

static void camera(void) {
  float cy = cosf_(V.yaw), sy = sinf_(V.yaw), cp = cosf_(V.pitch), sp = sinf_(V.pitch);
  cam_c[0] = V.dist * sy * cp, cam_c[1] = V.look + V.dist * sp, cam_c[2] = V.dist * cy * cp;
  cam_f[0] = -sy * cp, cam_f[1] = -sp, cam_f[2] = -cy * cp;
  cam_r[0] = cy, cam_r[1] = 0, cam_r[2] = -sy; /* f x up, normalized */
  cam_u[0] = -sy * sp, cam_u[1] = cp, cam_u[2] = -cy * sp;
  z_range();
}

/* The screen's box of the scene's sphere (a little larger: the poses), the
   scale it is drawn at, its projection and its tiles. A tile and a pixel or
   more around it fit the target, and every vertex of the sphere is in the
   guard band. */
static void ss_fit(void) {
  ss_s = 1, ss_k[0] = ss_k[1] = 2048;
  if (!ss_j)
    return;
  /* the centre (the world's origin) in view space */
  float r = grid_radius() * 1.08f;
  float xr = -(cam_c[0] * cam_r[0] + cam_c[1] * cam_r[1] + cam_c[2] * cam_r[2]);
  float yu = -(cam_c[0] * cam_u[0] + cam_c[1] * cam_u[1] + cam_c[2] * cam_u[2]);
  float zf = -(cam_c[0] * cam_f[0] + cam_c[1] * cam_f[1] + cam_c[2] * cam_f[2]);
  float lo[2] = {-GUARD, -GUARD}, hi[2] = {SW + GUARD, SH + GUARD};
  if (zf > r * 1.01f) {
    /* the planes through the eye that touch the sphere */
    float q = zf * zf - r * r, c[2] = {xr, yu};
    for (int a = 0; a < 2; a++) {
      float e = r * __builtin_sqrtf(c[a] * c[a] + q), t0 = (c[a] * zf - e) / q,
            t1 = (c[a] * zf + e) / q;
      if (a == 0)
        lo[0] = SW / 2 + FX * t0, hi[0] = SW / 2 + FX * t1;
      else
        lo[1] = SH / 2 - FY * t1, hi[1] = SH / 2 - FY * t0;
    }
  }
  float far = 1;
  for (int a = 0; a < 2; a++) {
    float n = a ? SH : SW;
    s32 b0 = (s32)clampf(lo[a], 0, n - 1), b1 = (s32)clampf(hi[a] + 1, (float)b0 + 1, n);
    ss_box[a] = (u32)b0, ss_box[a + 2] = (u32)b1;
    ss_c[a] = itof(b0 + b1) * 0.5f;
    float fl = ss_c[a] - lo[a], fh = hi[a] - ss_c[a];
    far = fl > far ? fl : far, far = fh > far ? fh : far;
  }
  ss_s = 2 * far > GUARD ? GUARD / far : 2;
  float dl = (ss_s - 1) * 0.5f;
  ss_k[0] = 2048 + dl + ss_s * (SW / 2 - ss_c[0]);
  ss_k[1] = 2048 + dl + ss_s * (SH / 2 - ss_c[1]);
  for (int a = 0; a < 2; a++) {
    float room = itof((a ? SS_H : SS_W) - 4) / ss_s;
    ss_tiles[a] = (u32)(itof((s32)(ss_box[a + 2] - ss_box[a])) / room) + 1;
  }
}

/* Tile (tx, ty)'s screen box, its centre c, and the move of the target's
   XYOFFSET that puts c at the target's centre (16ths). The target's pixel at
   the screen's (x, y) is then SS_W / 2 + ss_s (x - c) + (ss_s - 1) / 2 + e
   across: the ss_s x ss_s pixels of a screen pixel around its centre, e
   (pixels) what the move is off by its rounding to a 16th. */
static void ss_tile(u32 tx, u32 ty, u32 *box, float *c, s32 *off, float *e) {
  for (int a = 0; a < 2; a++) {
    u32 b0 = ss_box[a], w = ss_box[a + 2] - b0, n = ss_tiles[a], i = a ? ty : tx;
    box[a] = b0 + w * i / n, box[a + 2] = b0 + w * (i + 1) / n;
    c[a] = itof((s32)(box[a] + box[a + 2])) * 0.5f;
    float d = ss_s * (c[a] - ss_c[a]) * 16;
    off[a] = (s32)(d < 0 ? d - 0.5f : d + 0.5f);
    e[a] = (d - itof(off[a])) * (1 / 16.0f);
  }
}

/* Where pass p of n samples within a target pixel (16ths, x and y): n
   rooks, no two in a row or a column (4: the rotated grid). */
static void ss_jitter(u32 p, u32 n, s32 *dx, s32 *dy) {
  static const u8 row4[4] = {1, 3, 0, 2}, row8[8] = {0, 5, 2, 7, 4, 1, 6, 3};
  u32 q = n == 4 ? row4[p] : n == 8 ? row8[p] : p;
  *dx = (s32)((2 * p + 1) * 16 / n) / 2 - 8;
  *dy = (s32)((2 * q + 1) * 16 / n) / 2 - 8;
}

/* Supersampling with n samples a pixel (4, 8, 16, 32), 0: off. */
static void ss_set(u32 n) {
  ss_j = (int)(n / 4);
  if (n)
    mx_rt_init(&ss_rt, mx_gs.zbp, SS_W, SS_H, MX_GS_PSM_CT32,
               mx_gs.zbp + mx_rt_pages(SS_W, SS_H, MX_GS_PSM_CT32), MX_GS_PSM_Z24);
}

/* A direction of the world in model space (the model turned by V.spin). */
static void turn_back(const float *w, float *m) {
  float c = cosf_(V.spin), s = sinf_(V.spin);
  m[0] = c * w[0] - s * w[2], m[1] = w[1], m[2] = s * w[0] + c * w[2];
}

/* The MVP columns of a copy at (ox, 0, oz): the model turned by V.spin around
   y, scaled to radius R around its centre; P / P.w are GS coordinates, P.z /
   P.w = z_a / depth + z_b. Also the light and the eye in model space. */
static void mvp_of(float m[4][4], float ox, float oz, float light[3], float eye[3]) {
  float c = cosf_(V.spin), s = sinf_(V.spin), k = R / M.h->radius;
  /* model -> world: rows of the rotation times k, then the move */
  float W[3][4] = {{c * k, 0, s * k, 0}, {0, k, 0, 0}, {-s * k, 0, c * k, 0}};
  for (int a = 0; a < 3; a++)
    W[a][3] = (a == 0 ? ox : a == 2 ? oz : 0) -
              (W[a][0] * M.h->center[0] + W[a][1] * M.h->center[1] + W[a][2] * M.h->center[2]);
  float A[4][3];
  for (int a = 0; a < 3; a++) {
    A[0][a] = ss_k[0] * cam_f[a] + ss_s * FX * cam_r[a];
    A[1][a] = ss_k[1] * cam_f[a] - ss_s * FY * cam_u[a];
    A[2][a] = z_b * cam_f[a];
    A[3][a] = cam_f[a];
  }
  for (int j = 0; j < 4; j++)
    for (int r = 0; r < 4; r++) {
      float v = 0;
      for (int a = 0; a < 3; a++)
        v += A[r][a] * (W[a][j] - (j == 3 ? cam_c[a] : 0));
      m[j][r] = v;
    }
  m[3][2] += z_a;
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

static float uni[VU_IX_MSCEI_UNI_QW + 4][4] MX_ALIGNED(16);
static u32 cur_mat = ~0u;
static struct {
  u32 verts, indices, tris, draws;
  u32 skin, skinned; /* EE cycles of the skinning, vertices skinned */
  u32 direct;        /* draws sent without their chain (it did not fit) */
} st_frame;
static u64 skin_cyc; /* all of them, since the start */
static u32 skin_nv;

/* GS state for material m in pass (after the batches before it are drawn:
   a register written by PATH2 would reach the GS before their packets). */
static void gs_state(u32 m, int pass) {
  mx_frame_commit(mx_e_cnt(mx_frame_reserve(1), 0, mx_vif_flush(), 0));
  const struct mxm_mat *mt = &M.mat[m];
  const struct mx_tex *t = mip_mode == MIP_COLOURS ? &M.probe : &M.tex[m];
  /* K: log2 of the texels a pixel at w = 1 (the pixels a model unit: the
     scale to radius R and the projection's, across and down) */
  s32 k =
      mt->lod + mx_tex_lod_k(1.0f, ss_s * __builtin_sqrtf(FX * FY) * R / M.h->radius, 0) + mip_bias;
  u64 tex1 = mip_mode == MIP_OFF ? mx_gs_tex1(0, 0, 1, 1, 0, 0, 0)
             : mip_mode == MIP_COLOURS
                 ? mx_gs_tex1(0, (M.tex[m].levels < t->levels ? M.tex[m].levels : t->levels) - 1u,
                              0, 2, 0, 0, (u32)k)
                 : mx_tex_tex1(t, 1, mip_mode == MIP_TRI ? 5 : 4, 0, k);
  /* TFX HIGHLIGHT2: the texture times the colour, plus the colour's alpha
     (the highlight); the alpha drawn is the texture's */
  u64 *c = mx_frame_direct(9);
  c = mx_e_ad_tag(c, 8, 1);
  c = mx_tex_e_regs(c, t, 0, mx_tex_tex0(t, 1, 3), tex1);
  c = mx_e_ad(c, MX_GS_REG_CLAMP_1, mx_gs_clamp(0, 0, 0, 0, 0, 0));
  /* the alpha test: texels over half opaque (solid, and the blended pass
     that writes Z), or the others (the soft edges) */
  c = mx_e_ad(c, MX_GS_REG_TEST_1,
              mx_gs_test(1, pass == P_EDGE ? 3 : 6, 0x40, 0, 0, 0, 1, 2));
  c = mx_e_ad(c, MX_GS_REG_ZBUF_1,
              mx_gs_zbuf(ss_j ? ss_rt.zbp : mx_gs.zbp, MX_GS_PSM_Z24, pass == P_EDGE));
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
    /* the direction the animation itself gives the joint (its parents not
       moved by their springs): the limit is about it, so that the swings of
       a chain do not add up */
    float h0[3], t0[3], aw[3];
    xform(anim + 4 * r->joint, r->head, h0);
    xform(anim + 4 * r->joint, r->tip, t0);
    for (int k = 0; k < 3; k++)
      t0[k] -= h0[k];
    to_world(cs, sn, t0, aw);
    float awl = sqrtf_(dot3(aw, aw));
    float *x = P.x[i], *v = P.v[i], *tp = P.tp[i], tv[3];
    if (reset)
      for (int k = 0; k < 3; k++)
        x[k] = tw[k], v[k] = 0, tp[k] = tw[k];
    /* the damping slows the tip's speed relative to the animation's (what
       moves with the body is not held back, as it would be by air) */
    for (int k = 0; k < 3; k++)
      tv[k] = steps ? (tw[k] - tp[k]) / dt : 0, tp[k] = tw[k];
    for (int k = 0; k < 3; k++)
      a[k] = (tw[k] - hw[k]) / len, aw[k] = awl > 1e-6f ? aw[k] / awl : a[k];
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
      /* at the joint's length from the head, within limit of the animation's
         direction */
      for (int k = 0; k < 3; k++)
        b[k] = x[k] - hw[k];
      float bl = sqrtf_(dot3(b, b));
      if (bl < 1e-6f)
        for (int k = 0; k < 3; k++)
          b[k] = a[k];
      else
        for (int k = 0; k < 3; k++)
          b[k] /= bl;
      ca = dot3(aw, b);
      if (ca < cl) {
        for (int k = 0; k < 3; k++)
          u[k] = b[k] - aw[k] * ca;
        float ul = sqrtf_(dot3(u, u));
        for (int k = 0; k < 3; k++)
          b[k] = ul > 1e-6f ? aw[k] * cl + u[k] / ul * sl : aw[k];
        ca = cl;
      }
      for (int k = 0; k < 3; k++)
        x[k] = hw[k] + b[k] * len;
      /* a pendulum: no speed along the joint; slowed where the limit holds it */
      float vb = dot3(v, b);
      for (int k = 0; k < 3; k++)
        v[k] = (v[k] - vb * b[k]) * (ca <= cl ? 0.5f : 1.0f);
    }
    /* the turn of the joint: from where its parents left it (a) to b */
    ca = dot3(a, b);
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
   sh_ee) and skin_nrm (NRM's V4-8). Only
   the layout drawn, each vertex once; with VU0 in macro mode (-mvu0): the
   joint matrices blended by the weights (the vertices of one joint and of
   two in loops of their own), the position and the normal moved by them,
   the normal made 127 long again.
   Frame n uses buffer n & 1: frame n - 1 may still be drawn from the other
   (frame n - 2 is done once frame n has begun). -static: the rest pose (the
   first animation).
   A buffer holds only what a pose draws (each material's levels of detail
   drawn, the outfit worn), its clusters and strip runs one after the other
   as they are skinned (sk_take): sk_at[buf][c] is where cluster c's
   vertices are, sk_rat[buf][r] strip run r's, sk_used[buf] the vertices
   taken. Their texture coordinates are copied along (skin_uv): a batch reads
   all of its streams from one place. */
#define MAX_SKIN (214u << 10) /* Queen Marika in FullClothes as strips: 217568 */
typedef float f32x4 __attribute__((vector_size(16)));
typedef s32 s32x4 __attribute__((vector_size(16)));
typedef u8 u8x4 __attribute__((vector_size(4)));
typedef s8 s8x4 __attribute__((vector_size(4)));
static f32x4 skin_pos[2][MAX_SKIN + 8] MX_ALIGNED(64);
static s8x4 skin_nrm[2][MAX_SKIN + 8] MX_ALIGNED(64);
static u32 skin_uv[2][MAX_SKIN + 8] MX_ALIGNED(64);
static u32 sk_at[2][MAX_CLUSTER], sk_rat[2][MAX_STRIP], sk_used[2];
static int skin_buf;     /* this frame's */
/* of each buffer: the number of the last frame that draws from it, + 1 (it
   may be written once mx_frame_done() has reached it) */
static u32 buf_drawn[2];

/* This frame's joint matrices (four columns), the springs' where they move
   a joint, with the bind pose's V3-16 folded in (mxm.h qsize, qoff): joint
   j moves the position q / 32768 (ITOF15) to jq[j] (q / 32768, 1). The
   EE's, je: jq with the 1 / 32768 in its columns and the 1 / 65536 of the
   way it reads q (skin_pos_in). */
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
   (mflat has none); strips: w 2056.0 where the normal's w is set (a strip
   restarts there: XYZ2's ADC in the programs), else 0. */
MX_INLINE void skin_vertices(const s16 *__restrict pos, const s8x4 *__restrict nrm,
                             const u32 *__restrict jnt, const u8x4 *__restrict wgt,
                             const f32x4 (*__restrict pm)[4], f32x4 *__restrict op,
                             s8x4 *__restrict on, u32 n1, u32 n2, u32 n, const int normals,
                             const int strips) {
  u32 i = 0;
  /* one joint: its matrix as it is (the normal made 127 long as in the
     other loops: strip runs, not sorted, take theirs for all vertices) */
  SKIN_LOOP
  for (; i < n1; i++) {
    const f32x4 *m = pm[jnt[i] & 255];
    f32x4 p = skin_pos_in(pos + 3 * i);
    f32x4 r = m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3];
    if (strips)
      r[3] = nrm[i][3] ? 2056.0f : 0.0f;
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
    if (strips)
      r[3] = nrm[i][3] ? 2056.0f : 0.0f;
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
    if (strips)
      r[3] = nrm[i][3] ? 2056.0f : 0.0f;
    op[i] = r;
    if (normals) {
      f32x4 nv = skin_nrm_in(nrm + i);
      on[i] = skin_nrm_out(skin_unit(c0 * nv[0] + c1 * nv[1] + c2 * nv[2]));
    }
  }
}
/* Room for n vertices in buffer buf (from a multiple of 8: the VIF reads
   the streams from quadwords). */
static u32 sk_take(int buf, u32 n) {
  u32 at = sk_used[buf];
  sk_used[buf] += (n + 7) & ~7u;
  return at;
}

/* The texture coordinates of vertices first .. first + n - 1 (strips: of
   the strip streams) to at in buffer buf. */
static void skin_uvs(int buf, u32 at, u32 first, u32 n, int strips) {
  const u32 *uv = (const u32 *)sec(strips ? MXM_SUV : MXM_UV) + first;
  u32 *d = skin_uv[buf] + at;
  for (u32 i = 0; i < n; i++)
    d[i] = uv[i];
}

/* Vertices first .. first + n - 1 of the cluster or strip streams, of
   palette pal (skin_vertices: n1, n2), to at in buffer buf. */
static void skin_run(int buf, u32 at, u32 first, u32 n1, u32 n2, u32 n, u32 pal, int strips,
                     int normals) {
  int o = strips ? MXM_SPOS - MXM_POS : 0;
  skin_uvs(buf, at, first, n, strips);
  const s16 *pos = (const s16 *)sec(MXM_RPOS + o) + 3 * first;
  const s8x4 *nrm = (const s8x4 *)sec(MXM_RNRM + o) + first;
  const u32 *jnt = (const u32 *)sec(MXM_JNT + o) + first;
  const u8x4 *wgt = (const u8x4 *)sec(MXM_WGT + o) + first;
  /* strip runs: the skeleton's joints */
  const f32x4(*pm)[4] = pal == 0xFFFF ? (const f32x4(*)[4])je : pal_joints(pal);
  f32x4 *op = skin_pos[buf] + at;
  s8x4 *on = skin_nrm[buf] + at;
  if (strips && normals)
    skin_vertices(pos, nrm, jnt, wgt, pm, op, on, n1, n2, n, 1, 1);
  else if (strips)
    skin_vertices(pos, nrm, jnt, wgt, pm, op, on, n1, n2, n, 0, 1);
  else if (normals)
    skin_vertices(pos, nrm, jnt, wgt, pm, op, on, n1, n2, n, 1, 0);
  else
    skin_vertices(pos, nrm, jnt, wgt, pm, op, on, n1, n2, n, 0, 0);
}

/* ---- skinning with VU0 micro programs ----------------------------------------------- */

/* The clusters skinned by VU0 micro programs (demos/model/vu0skin.vsm,
   probe/p16_vu0reg.c), fed through VF registers: the EE moves a vertex into
   one of two register sets (QMTC2) while VU0 runs the program of the other,
   and takes the results out (SQC2, QMFC2); a program of the vertex's joint
   count (one, two, four: the cluster's n1, n2 and the rest) also ends the
   other set's vertex, so a normal comes out one call after its position.
   The palette is in VU0 data memory (column k of slot j at 32 k + j). A
   cluster's streams come into the scratchpad by toSPR (a chain of four
   refs) while the cluster before is skinned. Strips, and -skin macro: the
   loops above (VU0 macro mode). */
extern const u8 vu0skin_vu[], vu0skin_vu_end[];
enum { VS_P1A = 64, VS_P1B = 128, VS_P2A = 192, VS_P2B = 256, VS_P4A = 320, VS_P4B = 384 };
static int skin_micro = 1;
static u32 vu0_pal = ~0u, vu0_pal_frames; /* the palette in VU0 memory, of joints_frames */
static f32x4 vu0_cols[4][32] MX_ALIGNED(64);

#define VCALLMS(a) __asm__ volatile(".word %0" : : "i"(0x4a000038 | (a) << 6) : "memory")
#define VU0_WAIT() __asm__ volatile(".word 0x4848e801" : : : "$8", "memory") /* cfc2.i $8, vi29 */
/* A vertex into set A (vf1..vf4) or B (vf7..vf10) as MMI words: P q * 65536
   (PEXTLH with 0), N n * 2^24; two and four joints: J and W the bytes
   zero-extended (PEXTLB, PEXTLH with 0); one joint: J the raw word (the
   program takes its low byte), no W. */
#define IN_SET(IN, ...) IN(__VA_ARGS__)
#define SET_A "vf1", "vf2", "vf3", "vf4"
#define SET_B "vf7", "vf8", "vf9", "vf10"
#define VS_IN4(P, Nn, J, W, p, n, j, w)                                                            \
  __asm__ volatile("ldl $8, 7(%0)\n\tldr $8, 0(%0)\n\tlw $9, 0(%1)\n\tlw $10, 0(%2)\n\t"           \
                   "lw $11, 0(%3)\n\tpextlh $8, $8, $0\n\tpextlb $9, $9, $0\n\t"                   \
                   "pextlb $10, $0, $10\n\tpextlb $11, $0, $11\n\tpextlh $9, $9, $0\n\t"           \
                   "pextlh $10, $0, $10\n\tpextlh $11, $0, $11\n\t"                                \
                   "qmtc2 $8, $" p "\n\tqmtc2 $9, $" n "\n\tqmtc2 $10, $" j "\n\tqmtc2 $11, $" w   \
                   :                                                                               \
                   : "r"(P), "r"(Nn), "r"(J), "r"(W)                                               \
                   : "$8", "$9", "$10", "$11")
#define VS_IN1(P, Nn, J, W, p, n, j, w)                                                            \
  (void)(W);                                                                                       \
  __asm__ volatile("ldl $8, 7(%0)\n\tldr $8, 0(%0)\n\tlw $9, 0(%1)\n\tlw $10, 0(%2)\n\t"           \
                   "pextlh $8, $8, $0\n\tpextlb $9, $9, $0\n\tpextlh $9, $9, $0\n\t"               \
                   "qmtc2 $8, $" p "\n\tqmtc2 $9, $" n "\n\tqmtc2 $10, $" j                        \
                   :                                                                               \
                   : "r"(P), "r"(Nn), "r"(J)                                                       \
                   : "$8", "$9", "$10")
#define VS_IN2 VS_IN4
/* R of a set's vertex (SQC2), M (QMFC2, PPACH, PPACB, SW) */
#define VS_R(at, r) __asm__ volatile("sqc2 $" r ", 0(%0)" : : "r"(at) : "memory")
#define VS_M(at, m)                                                                                \
  __asm__ volatile("qmfc2 $8, $" m "\n\tppach $8, $0, $8\n\tppacb $8, $0, $8\n\tsw $8, 0(%0)"      \
                   :                                                                               \
                   : "r"(at)                                                                       \
                   : "$8", "memory")

/* The pipeline between calls: where R of the last vertex goes, M of it and
   of the one before; the set of the next vertex (0: A). Before the first
   vertex, they point at a sink. */
static struct {
  void *r, *m1, *m2;
  u32 set;
} VS;
static f32x4 vs_sink[2] MX_ALIGNED(16);

/* A vertex into set A, then the outputs of the two before it: R of the
   last (B: vf11) and M of the one before (A, ended by B's program: vf6);
   for B the other way round. */
#define VS_STEP_A(IN, PROG, i)                                                                     \
  IN_SET(IN, pp + 6 * (i), np + 4 * (i), jp + 4 * (i), wp + 4 * (i), SET_A);                       \
  VCALLMS(PROG);                                                                                   \
  VS_R(r, "vf11");                                                                                 \
  VS_M(m2, "vf6");                                                                                 \
  m2 = m1, m1 = om + (i), r = op + (i)
#define VS_STEP_B(IN, PROG, i)                                                                     \
  IN_SET(IN, pp + 6 * (i), np + 4 * (i), jp + 4 * (i), wp + 4 * (i), SET_B);                       \
  VCALLMS(PROG);                                                                                   \
  VS_R(r, "vf5");                                                                                  \
  VS_M(m2, "vf12");                                                                                \
  m2 = m1, m1 = om + (i), r = op + (i)
/* n vertices of one joint count: streams pp (RPOS), np, jp, wp; outputs op,
   om. */
#define VS_SEG(K)                                                                                  \
  static void vs_seg##K(const u8 *pp, const u8 *np, const u8 *jp, const u8 *wp, f32x4 *op,         \
                        s8x4 *om, u32 n) {                                                         \
    void *r = VS.r, *m1 = VS.m1, *m2 = VS.m2;                                                      \
    u32 i = 0;                                                                                     \
    if (VS.set && n) {                                                                             \
      VS_STEP_B(VS_IN##K, VS_P##K##B, 0);                                                          \
      i = 1;                                                                                       \
    }                                                                                              \
    for (; i + 1 < n; i += 2) {                                                                    \
      VS_STEP_A(VS_IN##K, VS_P##K##A, i);                                                          \
      VS_STEP_B(VS_IN##K, VS_P##K##B, i + 1);                                                      \
    }                                                                                              \
    if (i < n) {                                                                                   \
      VS_STEP_A(VS_IN##K, VS_P##K##A, i);                                                          \
    }                                                                                              \
    VS.r = r, VS.m1 = m1, VS.m2 = m2, VS.set = (VS.set + n) & 1;                                   \
  }
VS_SEG(1)
VS_SEG(2)
VS_SEG(4)

/* The palette into VU0 memory (once a pose), 127 for the normals' length
   (vf31.x), the pipeline empty. */
static void vs_begin(u32 pal) {
  if (vu0_pal != pal || vu0_pal_frames != joints_frames) {
    const f32x4(*pm)[4] = pal_joints(pal);
    for (u32 j = 0; j < JOINTS; j++)
      for (int k = 0; k < 4; k++)
        vu0_cols[k][j] = pm[j][k];
    mx_vu0_write(0, vu0_cols, 4 * 32);
    vu0_pal = pal, vu0_pal_frames = joints_frames;
  }
  static const float c127[4] MX_ALIGNED(16) = {127, 0, 0, 0};
  __asm__ volatile("lqc2 $vf31, 0(%0)" : : "r"(c127) : "memory");
  VS.r = VS.m1 = VS.m2 = vs_sink, VS.set = 0;
}
/* The last vertex out: a call of the next set (its tail ends it), the
   outputs, VU0 done. */
static void vs_end(void) {
  void *r = VS.r, *m2 = VS.m1;
  if (VS.set) {
    VCALLMS(VS_P1B);
    VS_R(r, "vf5");
    VS_M(VS.m2, "vf12");
    VU0_WAIT();
    VS_M(m2, "vf6");
  } else {
    VCALLMS(VS_P1A);
    VS_R(r, "vf11");
    VS_M(VS.m2, "vf6");
    VU0_WAIT();
    VS_M(m2, "vf12");
  }
}

/* The streams of cluster c into scratchpad buffer b (toSPR: a ref of each,
   from the quadword it starts in), the places of its vertices there. */
#define VS_SPR ((u8 *)0x70000000) /* the first 8 KB (FRAME_SPR: the others) */
static u64x2 vs_tags[2][4] MX_ALIGNED(64);
static const u8 *vs_at[2][4];
static void vs_stage(int b, u32 c) {
  static const u8 bytes[4] = {6, 4, 4, 4};
  u32 first = M.cl[c].first, nv = M.cl[c].nv, at = 0;
  u64x2 *t = MX_UNCACHED(vs_tags[b]);
  u8 *dst = VS_SPR + 1024 * (u32)b;
  for (int k = 0; k < 4; k++) {
    const u8 *a = (const u8 *)sec(MXM_RPOS + k) + bytes[k] * first;
    const u8 *a0 = (const u8 *)((uintptr_t)a & ~(uintptr_t)15);
    u32 q = (u32)(a + bytes[k] * nv - a0 + 15) / 16;
    t[k] = (u64x2){mx_dmatag(k < 3 ? MX_TAG_REF : MX_TAG_REFE, q, a0, 0), 0};
    vs_at[b][k] = dst + at + (a - a0);
    at += 16 * q;
  }
  MX_DMA_REG(MX_CH_TOSPR, SADR) = 1024 * (u32)b;
  MX_DMA_REG(MX_CH_TOSPR, QWC) = 0;
  MX_DMA_REG(MX_CH_TOSPR, TADR) = mx_phys(vs_tags[b]);
  mx_sync();
  MX_DMA_REG(MX_CH_TOSPR, CHCR) = MX_CHCR_CHAIN | MX_CHCR_STR;
}
_Static_assert(MXM_RNRM == MXM_RPOS + 1 && MXM_JNT == MXM_RPOS + 2 && MXM_WGT == MXM_RPOS + 3,
               "vs_stage takes the streams in this order");

/* Clusters c0 .. c1 - 1 (of palette pal) into buffer buf, each where
   sk_take puts it. */
static void skin_clusters(int buf, u32 pal, u32 c0, u32 c1, int normals) {
  if (!skin_micro) {
    for (u32 c = c0; c < c1; c++)
      skin_run(buf, sk_at[buf][c] = sk_take(buf, M.cl[c].nv), M.cl[c].first, cl_n1[c], cl_n2[c],
               M.cl[c].nv, pal, 0, normals);
    return;
  }
  if (c0 >= c1)
    return;
  vs_begin(pal);
  int b = 0;
  vs_stage(0, c0);
  for (u32 c = c0; c < c1; c++, b ^= 1) {
    mx_dma_wait(MX_CH_TOSPR);
    if (c + 1 < c1)
      vs_stage(b ^ 1, c + 1);
    const u8 *const *s = vs_at[b];
    u32 first = M.cl[c].first, n1 = cl_n1[c], n2 = cl_n2[c], n = M.cl[c].nv;
    u32 at = sk_at[buf][c] = sk_take(buf, n);
    skin_uvs(buf, at, first, n, 0);
    f32x4 *op = skin_pos[buf] + at;
    s8x4 *om = skin_nrm[buf] + at;
    vs_seg1(s[0], s[1], s[2], s[3], op, om, n1);
    vs_seg2(s[0] + 6 * n1, s[1] + 4 * n1, s[2] + 4 * n1, s[3] + 4 * n1, op + n1, om + n1, n2);
    u32 k = n1 + n2;
    vs_seg4(s[0] + 6 * k, s[1] + 4 * k, s[2] + 4 * k, s[3] + 4 * k, op + k, om + k, n - k);
  }
  vs_end();
}

static float fabsf_(float a) { return a < 0 ? -a : a; }
/* Whether vertex v of buffers 0 and 1 differ (bad: the ones before; the
   first three are printed). */
static int skin_differs(u32 v, u32 bad) {
  int d = 0;
  for (int f = 0; f < 3; f++) {
    float a = skin_pos[0][v][f], b = skin_pos[1][v][f], m = fabsf_(a) > 1 ? fabsf_(a) : 1;
    int dn = skin_nrm[0][v][f] - skin_nrm[1][v][f];
    d |= fabsf_(a - b) > m * (1.0f / 262144.0f) || dn > 1 || dn < -1;
  }
  if (d && bad < 3)
    mx_printf("model: skin check: vertex %u: %d %d %d / 1000 (%d %d %d) vs %d %d %d (%d %d %d)\n",
              v, (int)(skin_pos[0][v][0] * 1000), (int)(skin_pos[0][v][1] * 1000),
              (int)(skin_pos[0][v][2] * 1000), skin_nrm[0][v][0], skin_nrm[0][v][1],
              skin_nrm[0][v][2], (int)(skin_pos[1][v][0] * 1000), (int)(skin_pos[1][v][1] * 1000),
              (int)(skin_pos[1][v][2] * 1000), skin_nrm[1][v][0], skin_nrm[1][v][1],
              skin_nrm[1][v][2]);
  return d;
}

/* All clusters skinned both ways (a frame of the first animation), a group
   at a time: macro mode into buffer 0, the micro programs into 1; the
   vertices that differ (positions by more than 2^-18 of their size, normals
   by more than 1). */
static u32 skin_check(void) {
  if (!M.skinned || !M.h->ncluster)
    return 0;
  joints_frame(0, 7);
  int micro = skin_micro;
  u32 bad = 0, nv = 0;
  for (u32 gi = 0; gi < ngrp[1]; gi++) {
    const struct group *q = &grp[1][gi];
    sk_used[0] = sk_used[1] = 0;
    skin_micro = 0;
    skin_clusters(0, q->pal, q->c, q->e, 1);
    skin_micro = 1;
    skin_clusters(1, q->pal, q->c, q->e, 1);
    for (u32 c = q->c; c < q->e; c++)
      for (u32 i = 0; i < M.cl[c].nv; i++, nv++)
        bad += skin_differs(sk_at[0][c] + i, bad);
  }
  skin_micro = micro;
  sk_used[0] = sk_used[1] = 0;
  mx_printf("model: VU0 micro skinning against macro mode: %u of %u vertices differ\n", bad, nv);
  return bad;
}

/* The most vertices a pose may skin (sk_take's, either layout): of each
   outfit, each material at its largest level of detail (a model drawn
   once) or at all of them (copies, each at its own). */
static u32 skin_most(void) {
  u32 most = 0;
  for (u32 o = 0; o < (M.noutfit ? M.noutfit : 1); o++)
    for (int strips = 0; strips < 2; strips++) {
      static u32 at[MAX_MAT][16];
      for (u32 m = 0; m < MAX_MAT; m++)
        for (int l = 0; l < 16; l++)
          at[m][l] = 0;
      u32 n = strips ? M.h->nstrip : M.h->ncluster;
      for (u32 i = 0; i < n; i++) {
        u32 mat = strips ? M.strip[i].mat : M.cl[i].mat;
        u32 nv = strips ? M.strip[i].n : M.cl[i].nv;
        if ((strips ? run_mask(i) : cl_mask(i)) >> o & 1)
          at[mat & 0xFF][mat >> 8 & 15] += (nv + 7) & ~7u;
      }
      u32 sum = 0;
      for (u32 m = 0; m < M.h->nmat; m++) {
        u32 big = 0;
        for (int l = 0; l < 16; l++)
          big = M.h->flags & MXM_SINGLE ? (at[m][l] > big ? at[m][l] : big) : big + at[m][l];
        sum += big;
      }
      most = sum > most ? sum : most;
    }
  return most;
}

/* Buffer buf's vertices out of the data cache, for the DMA. */
static void skin_writeback(int buf, int normals) {
  mx_dcache_writeback(skin_pos[buf], sk_used[buf] * 16);
  mx_dcache_writeback(skin_uv[buf], sk_used[buf] * 4);
  if (normals)
    mx_dcache_writeback(skin_nrm[buf], sk_used[buf] * 4);
}

/* This frame's skinning (after mx_frame_begin and joints_frame), of the
   layout drawn. */
static void skin_frame(void) {
  st_frame.skin = st_frame.skinned = 0;
  if (!M.skinned)
    return;
  /* frame n - 2, which used this buffer, is done (or the frame of a pose
     that drew from it) */
  skin_buf = (int)(mx_frame_number() & 1);
  while (mx_frame_done() < buf_drawn[skin_buf])
    ;
  /* what is drawn (the rest pose too: the buffer holds only that) */
  int rest = !animated(V.shader);
  u32 t0 = mx_count(), n;
  int normals = rest || V.shader != S_FLAT;
  n = 0;
  sk_used[skin_buf] = 0;
  if (ix_now()) {
    for (u32 gi = 0; gi < ngrp[1]; gi++) {
      const struct group *q = &grp[1][gi];
      if (worn(q->mask) && (lod_need[q->mat & 0xFF] >> (q->mat >> 8) & 1)) {
        skin_clusters(skin_buf, q->pal, q->c, q->e, normals);
        n += q->nv;
      }
    }
  } else {
    for (u32 r = 0; r < M.h->nstrip; r++)
      if (worn(run_mask(r)) && (lod_need[M.strip[r].mat & 0xFF] >> (M.strip[r].mat >> 8) & 1)) {
        u32 at = sk_rat[skin_buf][r] = sk_take(skin_buf, M.strip[r].n);
        skin_run(skin_buf, at, M.strip[r].first, 0, 0, M.strip[r].n, M.strip[r].pal, 1, normals);
        n += M.strip[r].n;
      }
  }
  skin_writeback(skin_buf, normals);
  st_frame.skin = mx_count() - t0, st_frame.skinned = n;
  skin_cyc += st_frame.skin, skin_nv += n;
}

/* ---- poses: the animation apart from the frames -------------------------------------- */

/* -pose <k> (2): a pose (the animation, the springs, the joint matrices) is
   made at least every k frames, and as often as every frame when the EE has
   the time: it skins the pose into the buffer not drawn (the back one) in
   chunks, while it waits for ring space or for the back framebuffer
   (mx_frame_idle), and at the end of each frame up to its share (1 / k of
   the pose a frame). Each frame draws the newest complete pose; the camera
   and the turning still move every frame (VU1 applies them). A frame that
   needs what the pose drawn lacks (a level of detail, the layout, the
   normals) first completes the pose being skinned, with that added.
   -pose 1, the rest pose and the pictures compared: each frame skins
   its own pose first (skin_frame). */
static int pose_frames = 2;
static struct {
  int on;                  /* a pose is being skinned into the back buffer */
  int layout, normals;     /* the job's: 1 + indexed; with normals */
  u32 af;                  /* the job's animation frame */
  u16 need[MAX_MAT];       /* the job's levels of each material (lod_need) */
  u32 unit, k;             /* the next group (indexed) or strip run, its next cluster */
  int rescan;              /* levels were added: once more over the units */
  u32 total, done, frames; /* vertices to skin, skinned; frames since it began */
  u32 taken;               /* of the back buffer, by the units of its levels */
  int front, front_ok;     /* the buffer drawn, a complete pose in it */
  int front_layout, front_normals;
  u16 front_need[MAX_MAT];
  u32 poses; /* completed */
} J;
static u8 unit_done[MAX_GROUP > MAX_STRIP ? MAX_GROUP : MAX_STRIP];

static u32 units(int layout) { return layout == 2 ? ngrp[1] : M.h->nstrip; }
static u32 unit_mat(int layout, u32 u) { return layout == 2 ? grp[1][u].mat : M.strip[u].mat; }
static u32 unit_nv(int layout, u32 u) { return layout == 2 ? grp[1][u].nv : M.strip[u].n; }
static u32 unit_mask(int layout, u32 u) { return layout == 2 ? grp[1][u].mask : run_mask(u); }
static int unit_needed(u32 u) {
  u32 mat = unit_mat(J.layout, u);
  return worn(unit_mask(J.layout, u)) && (J.need[mat & 0xFF] >> (mat >> 8) & 1);
}

/* Up to about budget vertices more of the job (whole clusters and strip
   runs); returns whether it is complete (job_step(0): only that). */
static int job_step(u32 budget) {
  int back = J.front ^ 1;
  u32 nu = units(J.layout), n = 0, t0 = mx_count();
  for (;;) {
    if (J.unit >= nu) {
      if (!J.rescan)
        break;
      J.rescan = 0, J.unit = J.k = 0;
      continue;
    }
    u32 u = J.unit;
    if (unit_done[u] || !unit_needed(u)) {
      J.unit++, J.k = 0;
      continue;
    }
    if (n >= budget)
      break;
    if (J.layout == 2) {
      const struct group *q = &grp[1][u];
      u32 c0 = q->c + J.k, c1 = c0;
      for (; c1 < q->e && n < budget; c1++)
        n += M.cl[c1].nv;
      skin_clusters(back, q->pal, c0, c1, J.normals);
      J.k = c1 - q->c;
      if (c1 < q->e)
        break;
    } else {
      const struct mxm_strip *r = &M.strip[u];
      u32 at = sk_rat[back][u] = sk_take(back, r->n);
      skin_run(back, at, r->first, 0, 0, r->n, r->pal, 1, J.normals);
      n += r->n;
    }
    unit_done[u] = 1, J.unit++, J.k = 0;
  }
  J.done += n;
  u32 dt = mx_count() - t0;
  st_frame.skin += dt, st_frame.skinned += n;
  skin_cyc += dt, skin_nv += n;
  return J.unit >= nu && !J.rescan;
}

/* The back buffer may be written: the frames that drew from it are done. */
static int back_free(void) { return mx_frame_done() >= buf_drawn[J.front ^ 1]; }

/* The vertices of the units of the levels need has and the job's not, as
   the buffer takes them (sk_take). */
static u32 need_nv(const u16 *need) {
  u32 n = 0;
  for (u32 u = 0; u < units(J.layout); u++) {
    u32 mat = unit_mat(J.layout, u);
    if (!((need[mat & 0xFF] & ~J.need[mat & 0xFF]) >> (mat >> 8) & 1 &&
          worn(unit_mask(J.layout, u))))
      continue;
    if (J.layout == 2)
      for (u32 c = grp[1][u].c; c < grp[1][u].e; c++)
        n += (M.cl[c].nv + 7) & ~7u;
    else
      n += (M.strip[u].n + 7) & ~7u;
  }
  return n;
}

static void job_need(const u16 *need) {
  /* levels that changed while the job was being made: when the buffer
     cannot hold them with the job's, the job begins again with need only
     (which fits: skin_most) */
  if (J.taken + need_nv(need) > MAX_SKIN) {
    for (u32 u = 0; u < units(J.layout); u++)
      unit_done[u] = 0;
    for (u32 mt = 0; mt < M.h->nmat; mt++)
      J.need[mt] = 0;
    J.unit = J.k = 0, J.total = J.done = J.taken = 0;
    sk_used[J.front ^ 1] = 0;
  }
  J.taken += need_nv(need);
  for (u32 mt = 0; mt < M.h->nmat; mt++)
    if (need[mt] & ~J.need[mt]) {
      u16 add = need[mt] & ~J.need[mt];
      J.need[mt] |= add;
      for (u32 u = 0; u < units(J.layout); u++) {
        u32 mat = unit_mat(J.layout, u);
        if ((mat & 0xFF) == mt && (add >> (mat >> 8) & 1) && worn(unit_mask(J.layout, u)))
          J.total += unit_nv(J.layout, u);
      }
      J.rescan = 1;
    }
}

/* The joint matrices of the job and of the front pose. */
static f32x4 jq_job[MAX_JOINTS][4] MX_ALIGNED(16), jq_front[MAX_JOINTS][4] MX_ALIGNED(16);

/* Whether this pose's joint matrices (jq) are the front pose's, to within
   1e-5 of the model's radius (a vertex moves less than about 4e-5 of it). */
static int pose_same(void) {
  float eps = 1e-5f * M.h->radius;
  for (u32 j = 0; j < M.h->njoint; j++)
    for (int c = 0; c < 4; c++)
      for (int k = 0; k < 3; k++)
        if (fabsf_(jq[j][c][k] - jq_front[j][c][k]) > eps)
          return 0;
  return 1;
}

/* A new pose at time t, animation frame af, into the back buffer; none
   when the front pose is the same and has what the frame needs (a still
   pose, its springs at rest: not skinned again). */
static void job_start(float t, u32 af, int layout, int normals) {
  if (!(P.n && !P.reset && t == P.t)) /* the springs at t already */
    phys_update(af, t);
  joints_frame((u32)V.anim, af);
  int same = J.front_ok && J.front_layout == layout && J.front_normals >= normals;
  for (u32 mt = 0; mt < M.h->nmat && same; mt++)
    same = !(lod_need[mt] & ~J.front_need[mt]);
  if (same && pose_same()) {
    J.on = 0;
    return;
  }
  for (u32 j = 0; j < M.h->njoint; j++)
    for (int c = 0; c < 4; c++)
      jq_job[j][c] = jq[j][c];
  J.on = 1, J.layout = layout, J.normals = normals, J.af = af;
  J.unit = J.k = 0, J.rescan = 0, J.total = J.done = J.frames = J.taken = 0;
  for (u32 u = 0; u < units(layout); u++)
    unit_done[u] = 0;
  for (u32 mt = 0; mt < M.h->nmat; mt++)
    J.need[mt] = 0;
  job_need(lod_need);
  J.rescan = 0;
  sk_used[J.front ^ 1] = 0;
}

/* The job completed (waiting for the back buffer) and made the front. */
static void job_finish(void) {
  int back = J.front ^ 1;
  while (!back_free())
    ;
  while (!job_step(~0u))
    ;
  skin_writeback(back, J.normals);
  J.front = back, J.front_ok = 1, J.front_layout = J.layout, J.front_normals = J.normals;
  for (u32 mt = 0; mt < M.h->nmat; mt++)
    J.front_need[mt] = J.need[mt];
  for (u32 j = 0; j < M.h->njoint; j++)
    for (int c = 0; c < 4; c++)
      jq_front[j][c] = jq_job[j][c];
  J.on = 0, J.poses++;
}

/* mx_frame_idle: a chunk of the job, once the back buffer is free. */
static int pose_idle(void) {
  if (!J.on || job_step(0))
    return 0;
  if (back_free())
    job_step(256);
  return 1;
}

/* This frame's pose (after lod_choose): the newest complete one, or one
   made now (see above). */
static void pose_frame(float t, u32 af) {
  st_frame.skin = st_frame.skinned = 0;
  int layout = 1 + ix_now(), normals = V.shader != S_FLAT;
  if (pose_frames <= 1 || !M.skinned || !animated(V.shader)) {
    J.on = J.front_ok = 0;
    phys_update(af, t);
    if (M.skinned)
      joints_frame(animated(V.shader) ? (u32)V.anim : 0, af);
    skin_frame();
    J.poses += animated(V.shader);
    buf_drawn[skin_buf] = mx_frame_number() + 1;
    J.front = skin_buf;
    return;
  }
  if (J.on && (J.layout != layout || J.normals < normals))
    J.on = 0;
  int lacks = !J.front_ok || J.front_layout != layout || J.front_normals < normals;
  for (u32 mt = 0; mt < M.h->nmat && !lacks; mt++)
    lacks = (lod_need[mt] & ~J.front_need[mt]) != 0;
  if (lacks) {
    if (!J.on)
      job_start(t, af, layout, normals);
    job_need(lod_need);
    job_finish();
  } else if (J.on && job_step(0)) {
    job_finish();
  }
  if (!J.on)
    job_start(t, af, layout, normals);
  J.frames++;
  skin_buf = J.front;
  buf_drawn[skin_buf] = mx_frame_number() + 1;
}

/* The end of the frame's draws: the job up to its share. */
static void pose_end(void) {
  if (!J.on)
    return;
  u32 share = J.frames >= (u32)pose_frames ? J.total : J.total / (u32)pose_frames * J.frames;
  if (J.done >= share)
    return;
  while (!back_free())
    ;
  job_step(share - J.done);
}

/* The streams of a shader's inputs; strips: the S* streams. */
static void streams_of(int shader, int strips, const void **st) {
  int o = strips ? MXM_SPOS - MXM_POS : 0;
  int k = 0;
  int sk = M.skinned; /* skin_frame's */
  st[k++] = sk ? (const void *)skin_pos[skin_buf] : sec(MXM_POS + o);
  if (shader >= S_LIT)
    st[k++] = sk ? (const void *)skin_nrm[skin_buf] : sec(MXM_NRM + o);
  if (shader >= S_TEX)
    st[k++] = sk ? (const void *)skin_uv[skin_buf] : sec(MXM_UV + o);
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

/* The uniforms of mscei and msceic (in uni after the MVP): the MVP without
   the screen offset (it adds it: prm), and its z of the guard band's size:
   t.z = Z_K (z - Z_MID) for the GS's z (z_a / depth + z_b): +-GUARD where z
   leaves 0 .. 2^20; the GS's z is t.z / Z_K + Z_MID (scl, prm). The key
   light (light, model space, as the other shaders), a fill from the other
   side and a rim light from behind, each modulating the texture (128 =
   1.0). */
#define Z_K (GUARD / Z_MID)
static void scei_uniforms(const float mvp[4][4], const float light[3]) {
  for (int j = 0; j < 4; j++) {
    uni[j][0] = mvp[j][0] - 2048 * mvp[j][3];
    uni[j][1] = mvp[j][1] - 2048 * mvp[j][3];
    uni[j][2] = Z_K * ((z_b - Z_MID) * mvp[j][3] + (j == 3 ? z_a : 0));
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
    uni[VU_IX_MSCEI_PRM][k] = k == 2 ? Z_MID : 2048;
  }
  uni[VU_IX_MSCEI_AMB][3] = 0; /* no highlight (TFX HIGHLIGHT2 adds the alpha) */
  uni[VU_IX_MSCEI_PRM][3] = GUARD;
  uni[VU_IX_MSCEI_SCL][3] = 0;
}

/* The uniforms of shader for material m. */
static void set_uniforms(int shader, const float mvp[4][4], const float light[3],
                         const float eye[3], u32 m) {
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
  for (int k = 0; k < 3; k++) {
    uni[VU_IX_MLIT_LIGHT][k] = light[k] / 127.0f;
    uni[VU_IX_MLIT_LCOL][k] = base[k] * 0.85f;
    uni[VU_IX_MLIT_AMB][k] = base[k] * 0.32f;
  }
  uni[VU_IX_MLIT_LIGHT][3] = uni[VU_IX_MLIT_LCOL][3] = 0;
  uni[VU_IX_MLIT_AMB][3] = 128;
  /* the highlight: the material's strength (mxm_mat color[3]) */
  for (int k = 0; k < 3; k++)
    uni[VU_IX_MLIT_EYE][k] = eye[k];
  uni[VU_IX_MLIT_EYE][3] = (float)M.mat[m].color[3];
}

_Static_assert(VU_IX_MLIT_LIGHT == VU_IX_MTEX_LIGHT && VU_IX_MLIT_LCOL == VU_IX_MTEX_LCOL &&
                   VU_IX_MLIT_AMB == VU_IX_MTEX_AMB && VU_IX_MLIT_EYE == VU_IX_MTEX_EYE,
               "the lit shaders share the uniforms' places");

/* ---- mesh chains --------------------------------------------------------------------- */

/* A draw's batches (a strip run, or a group of clusters) are built once a
   frame as a mesh chain (mx_shader_mesh), at the first copy that draws it,
   into the buffer of the frame's slot: free again two frames on, when
   mx_frame_begin has waited for that frame. The copies call it
   (mx_frame_shader_call): a copy sends its context and a CALL, whatever its
   size. The skinned vertices are where this frame's pose put them, the same
   for every copy. -mfifo: the frame stream through the MFIFO, each copy
   sending its batches (mx_frame_shader_draw / idraw). */
#ifndef MODEL_MFIFO
#define MODEL_MFIFO 0 /* -DMODEL_MFIFO=1: -mfifo by default (a run without arguments) */
#endif
static int use_mfifo = MODEL_MFIFO;
#define MESH_QW (6u << 10) /* per slot */
#define MAX_MESH (MAX_GROUP > MAX_STRIP ? MAX_GROUP : MAX_STRIP)
static u64 mesh_mem[2][2 * MESH_QW] MX_ALIGNED(64);
/* The slots' memory: mesh_mem, or halves of the rest of file_buf after the
   models when that is more (mesh_room): supersampling's tiles each build
   chains of their clusters. */
static u8 *mesh_at[2] = {(u8 *)mesh_mem[0], (u8 *)mesh_mem[1]};
static u32 mesh_bytes = sizeof mesh_mem[0];
static void mesh_room(u8 *p, u32 n) {
  u32 half = n / 2 & ~63u;
  if (half > mesh_bytes)
    mesh_at[0] = p, mesh_at[1] = p + half, mesh_bytes = half;
}
static struct mx_pbuf mesh_pb[2];
static u32 mesh_pb_frame[2]; /* the frame (+ 1) of the buffer's chains */
static struct mx_chain mesh[MAX_MESH];
static u32 mesh_frame[MAX_MESH]; /* the frame (+ 1) mesh[u] was built for */

/* The buffer for this frame's chains. */
static struct mx_pbuf *mesh_buf(void) {
  u32 f = mx_frame_number() + 1, k = f & 1;
  if (mesh_pb_frame[k] != f) {
    mx_pbuf_init(&mesh_pb[k], mesh_at[k], mesh_bytes);
    mesh_pb_frame[k] = f;
  }
  return &mesh_pb[k];
}
/* Unit u's chain of this frame if it has been built (an empty one if it did
   not fit: drawn directly then), else 0. */
static const struct mx_chain *mesh_built(u32 u) {
  return mesh_frame[u] == mx_frame_number() + 1 ? &mesh[u] : 0;
}

/* ---- supersampling's tiles ------------------------------------------------------------ */

/* The tiles each cluster reaches this frame (bit t: tile t, as ss_draw
   numbers them), of any copy: its sphere (of the skinned vertices, else the
   model's), projected, with a margin of 2 target pixels (the bilinear
   sprite's texel and the jitter). */
static u16 cl_tiles[MAX_CLUSTER];
/* A group's chain of the clusters of one tile, built at the tile's first
   draw (ss_tmesh_at: the frame and tile it is of), and its counts. */
static struct mx_chain tmesh[MAX_GROUP];
static u32 tmesh_at[MAX_GROUP], tmesh_n[MAX_GROUP], tmesh_nv[MAX_GROUP], tmesh_ni[MAX_GROUP],
    tmesh_tris[MAX_GROUP];
static u32 tile_stamp(void) { return (mx_frame_number() + 1) << 5 | (u32)(ss_cur + 1); }

/* Whether group q is drawn this frame by some copy (its level, the outfit). */
static int grp_drawn(const struct group *q) {
  return worn(q->mask) && (lod_need[q->mat & 0xFF] >> (q->mat >> 8) & 1);
}

/* Cluster k's sphere in model space (skinned: of this frame's vertices). */
static void cl_sphere(u32 k, float *sp) {
  const struct mxm_cluster *cl = &M.cl[k];
  if (!M.skinned) {
    for (int i = 0; i < 4; i++)
      sp[i] = cl->sphere[i];
    return;
  }
  const f32x4 *v = skin_pos[skin_buf] + sk_at[skin_buf][k];
  f32x4 lo = v[0], hi = v[0];
  for (u32 i = 1; i < cl->nv; i++)
    for (int a = 0; a < 3; a++) {
      lo[a] = v[i][a] < lo[a] ? v[i][a] : lo[a];
      hi[a] = v[i][a] > hi[a] ? v[i][a] : hi[a];
    }
  float r2 = 0;
  for (int a = 0; a < 3; a++) {
    float h = (hi[a] - lo[a]) * 0.5f;
    sp[a] = lo[a] + h, r2 += h * h;
  }
  sp[3] = sqrtf_(r2);
}

static void ss_bin(void) {
  u32 nt = ss_tiles[0] * ss_tiles[1];
  float tr[SS_MAX_TILES][4], dl = (ss_s - 1) * 0.5f, mg = 2;
  for (u32 t = 0; t < nt; t++) {
    u32 b[4];
    float c[2], e[2];
    s32 off[2];
    ss_tile(t % ss_tiles[0], t / ss_tiles[0], b, c, off, e);
    for (int a = 0; a < 4; a++)
      tr[t][a] = 2048 + dl + ss_s * (itof((s32)b[a]) - ss_c[a & 1]) + (a < 2 ? -mg : mg);
  }
  static float mv[MAX_COPIES][4][4];
  float light[3], eye[3], k = R / M.h->radius, step = 2.4f * R,
                          o = -step * itof(V.copies - 1) * 0.5f;
  for (int z = 0; z < V.copies; z++)
    for (int x = 0; x < V.copies; x++)
      mvp_of(mv[z * V.copies + x], o + step * itof(x), o + step * itof(z), light, eye);
  for (u32 gi = 0; gi < ngrp[0]; gi++) {
    const struct group *q = &grp[0][gi];
    if (!grp_drawn(q))
      continue;
    for (u32 cl = q->c; cl < q->e; cl++) {
      float sp[4];
      cl_sphere(cl, sp);
      u32 bits = 0;
      for (int i = 0; i < V.copies * V.copies && bits != (1u << nt) - 1; i++) {
        const float (*m)[4] = mv[i];
        float p[4];
        for (int r = 0; r < 4; r++)
          p[r] = m[0][r] * sp[0] + m[1][r] * sp[1] + m[2][r] * sp[2] + m[3][r];
        float rw = sp[3] * k, w = p[3] - rw;
        if (w < NEAR_Z) {
          bits = (1u << nt) - 1;
          break;
        }
        /* x (and y) of the sphere's points: the centre's, off by at most
           rw (s FX + |the centre's x from the axis|) / w */
        float gx = p[0] / p[3], gy = p[1] / p[3];
        float rx = rw * (ss_s * FX + fabsf_(gx - ss_k[0])) / w;
        float ry = rw * (ss_s * FY + fabsf_(gy - ss_k[1])) / w;
        for (u32 t = 0; t < nt; t++)
          if (gx + rx >= tr[t][0] && gx - rx <= tr[t][2] && gy + ry >= tr[t][1] &&
              gy - ry <= tr[t][3])
            bits |= 1u << t;
      }
      cl_tiles[cl] = (u16)bits;
    }
  }
}

/* The clusters of group q in tile ss_cur into out (where this frame's pose
   has them: ee), counted into tmesh_*[gi]; returns how many. */
static u32 tile_batches(u32 gi, const struct mx_ibatch *b, int ee, struct mx_ibatch *out) {
  const struct group *q = &grp[0][gi];
  u32 n = 0, nv = 0, ni = 0, tris = 0, bit = 1u << ss_cur;
  for (u32 k = q->c; k < q->e; k++)
    if (cl_tiles[k] & bit) {
      out[n] = b[k];
      if (ee)
        out[n].first = sk_at[skin_buf][k];
      n++, nv += M.cl[k].nv, ni += M.cl[k].ni, tris += cl_tris[k];
    }
  tmesh_n[gi] = n, tmesh_nv[gi] = nv, tmesh_ni[gi] = ni, tmesh_tris[gi] = tris;
  return n;
}

static void draw_copy(float ox, float oz, const u8 *lod) {
  int shader = V.shader, tex = shader >= S_TEX;
  float m[4][4], light[3], eye[3];
  mvp_of(m, ox, oz, light, eye);
  const struct mx_shader *s = &(M.skinned ? sh_ee : sh)[shader][ix_now()];
  /* AA1 only where nothing is blended */
  u32 prim_solid = (u32)mx_gs_prim(MX_GS_PRIM_TRISTRIP, 1, tex, 0, aa1, aa1, 0, 0, 0);
  u32 prim_blend = (u32)mx_gs_prim(MX_GS_PRIM_TRISTRIP, 1, tex, 0, 1, 0, 0, 0, 0);
  int npass = tex ? P_N : 1;
  const void *st[6];
  if (ix_now()) {
    streams_of(shader, 0, st);
    struct mx_ibatch *b = ibatch[s->out_qw == 3];
    /* skinned by the EE: each cluster where the pose's buffer has it */
    static struct mx_ibatch in[MAX_CLUSTER];
    int ee = M.skinned;
    /* the clusters of one material and level, one draw */
    const struct group *g = grp[0];
    for (int pass = 0; pass < npass; pass++)
      for (u32 gi = 0; gi < ngrp[0]; gi++) {
        const struct group *q = &g[gi];
        u32 mat = q->mat & 0xFFu, n = 0;
        if ((pass != P_SOLID) != (tex && blended(mat)) || q->mat >> 8 != lod[mat] || !worn(q->mask))
          continue;
        /* a tile's: only the clusters that reach it */
        int tiled = ss_cur >= 0 && ss_cull;
        const struct mx_chain *ch = use_mfifo ? 0
                                    : tiled   ? (tmesh_at[gi] == tile_stamp() ? &tmesh[gi] : 0)
                                              : mesh_built(gi);
        int call = ch && !mx_chain_empty(ch);
        const struct mx_ibatch *bat = tiled || ee ? in : b + q->c;
        if (tiled) {
          n = call ? tmesh_n[gi] : tile_batches(gi, b, ee, in);
          st_frame.verts += tmesh_nv[gi], st_frame.indices += tmesh_ni[gi];
          st_frame.tris += pass == P_EDGE ? 0 : tmesh_tris[gi];
        } else {
          n = q->e - q->c;
          if (ee && !call)
            for (u32 k = q->c; k < q->e; k++) {
              in[k - q->c] = b[k];
              in[k - q->c].first = sk_at[skin_buf][k];
            }
          st_frame.verts += q->nv, st_frame.indices += q->ni;
          st_frame.tris += pass == P_EDGE ? 0 : q->tris;
        }
        if (n) {
          if (!use_mfifo && !ch) {
            struct mx_chain *mc = tiled ? &tmesh[gi] : &mesh[gi];
            mx_shader_imesh(mc, mesh_buf(), s, st, bat, n);
            if (tiled)
              tmesh_at[gi] = tile_stamp();
            else
              mesh_frame[gi] = mx_frame_number() + 1;
            call = !mx_chain_empty(mc);
            ch = mc;
          }
          if (tex && (mat | (u32)pass << 8) != cur_mat)
            gs_state(mat, pass);
          bind(s);
          set_uniforms(shader, m, light, eye, mat);
          mx_dcache_writeback(uni, sizeof uni);
          if (call) {
            mx_frame_shader_call(s, pass ? prim_blend : prim_solid, uni, ch);
          } else {
            mx_frame_shader_idraw(s, pass ? prim_blend : prim_solid, uni, st, bat, n);
            st_frame.direct += !use_mfifo;
          }
          st_frame.draws++;
        }
      }
  } else {
    streams_of(shader, 1, st);
    for (int pass = 0; pass < npass; pass++)
      for (u32 r = 0; r < M.h->nstrip; r++) {
        const struct mxm_strip *sr = &M.strip[r];
        u32 mat = sr->mat & 0xFF;
        if ((pass != P_SOLID) != (tex && blended(mat)) || sr->mat >> 8 != lod[mat] ||
            !worn(run_mask(r)))
          continue;
        const void *at[6];
        u32 first = M.skinned ? sk_rat[skin_buf][r] : sr->first;
        for (u32 i = 0; i < s->nin; i++)
          at[i] = (const u8 *)st[i] + first * s->in[i].bytes;
        const struct mx_chain *ch = 0;
        if (!use_mfifo && !(ch = mesh_built(r))) {
          mx_shader_mesh(&mesh[r], mesh_buf(), s, prim_solid, at, sr->n);
          mesh_frame[r] = mx_frame_number() + 1;
          ch = &mesh[r];
        }
        if (tex && (mat | (u32)pass << 8) != cur_mat)
          gs_state(mat, pass);
        bind(s);
        set_uniforms(shader, m, light, eye, mat);
        mx_dcache_writeback(uni, sizeof uni);
        if (ch && !mx_chain_empty(ch))
          mx_frame_shader_call(s, pass ? prim_blend : prim_solid, uni, ch);
        else
          mx_frame_shader_draw(s, pass ? prim_blend : prim_solid, uni, at, sr->n);
        st_frame.verts += strip_cost(s, sr->n), st_frame.indices += sr->n;
        st_frame.tris += pass == P_EDGE ? 0 : run_tris[r];
        st_frame.draws++;
      }
  }
}

static char hud[4][128];
static int hud_keys; /* the interactive mode: the buttons shown */
static int no_hud;   /* pictures of the scene alone */

/* R3's steps: AA1, the display filter (MX_SMOOTH_*), supersampling's
   samples a pixel */
static const struct {
  u8 aa1, smooth, ss;
} quality[] = {{0, 0, 0},
               {1, 0, 0},
               {0, MX_SMOOTH_ACROSS, 0},
               {1, MX_SMOOTH_ACROSS, 0},
               {0, MX_SMOOTH_DOWN, 0},
               {1, MX_SMOOTH_DOWN, 0},
               {0, 0, 4},
               {0, 0, 16},
               {0, MX_SMOOTH_DOWN, 16}};
static u32 quality_at;
static void quality_set(u32 i) {
  quality_at = i;
  aa1 = quality[i].aa1;
  mx_gs_smooth(quality[i].smooth);
  ss_set(quality[i].ss);
}

/* name (16 bytes, maybe not ended) in capitals, a space before each capital
   after a small letter (FullClothes: FULL CLOTHES) */
static char *hud_name(char *d, const char *name) {
  for (int i = 0; i < 16 && name[i]; i++) {
    char ch = name[i];
    if (i && ch >= 'A' && ch <= 'Z' && name[i - 1] >= 'a' && name[i - 1] <= 'z')
      *d++ = ' ';
    *d++ = ch >= 'a' && ch <= 'z' ? (char)(ch - 32) : ch;
  }
  *d = 0;
  return d;
}

#define HUD_TEXT 0x808080u /* white */
#define HUD_GOLD 0x806A38u /* the outfit */
#define HUD_GREY 0x5C5C66u
#define HUD_DIM 0x44444Cu
#define HUD_PANEL 0x58000000u /* black, 69 % */

/* The HUD: at the top left the model, its outfit and animation; at the top
   right the buttons (interactive); at the bottom left the mode, the counts
   and the speed (hud[]). Lines of a 224-line screen. */
static void hud_draw(void) {
  char of[24] = "", an[24] = "";
  if (M.noutfit)
    hud_name(of, M.outfit[outfit]);
  if (animated(V.shader))
    hud_name(an, M.anim[V.anim].name);
  u32 wo = hud_width(of, 1), ax = 16 + (wo ? wo + 18 : 0);
  u32 w = hud_width(M.name, 2), ws = ax - 16 + hud_width(an, 1);
  w = ws > w ? ws : w;
  hud_box(8, 6, w + 16, *of || *an ? 34 : 24, HUD_PANEL);
  hud_text(16, 11, M.name, HUD_TEXT, 2);
  hud_text(16, 29, of, HUD_GOLD, 1);
  hud_text(ax, 29, an, HUD_GREY, 1);
  if (hud_keys) {
    /* 0: SELECT's and TRIANGLE's, which depend on the model */
    static const char *const keys[][2] = {{"SELECT", 0},
                                          {"LSTICK", "ZOOM"},
                                          {"RSTICK", "TURN"},
                                          {"CIRCLE", "ANIMATION"},
                                          {"SQUARE", "SHADER"},
                                          {"TRIANGLE", 0},
                                          {"CROSS", "STRIPS/INDEXED"},
                                          {"L3", "VIEW"},
                                          {"R3", "AA/FILTER/SS"}};
    const char *sel = M.noutfit > 1 ? "OUTFIT" : nmodel > 1 ? "MODEL" : 0;
    const char *tri = M.h->flags & MXM_SINGLE ? "SPRINGS" : "COPIES";
    u32 kw = hud_width("TRIANGLE  STRIPS/INDEXED", 1), x = SW - 8 - kw - 16, y = 11, n = 0;
    for (u32 i = 0; i < sizeof keys / sizeof *keys; i++)
      n += (keys[i][1] ? keys[i][1] : i ? tri : sel) != 0;
    hud_box(x, 6, kw + 16, 10 * n + 7, HUD_PANEL);
    for (u32 i = 0; i < sizeof keys / sizeof *keys; i++) {
      const char *what = keys[i][1] ? keys[i][1] : i ? tri : sel;
      if (!what)
        continue;
      hud_text(x + 8, y, keys[i][0], HUD_GREY, 1);
      hud_text(x + 8 + 6 * 10, y, what, HUD_TEXT, 1);
      y += 10;
    }
  }
  u32 bw = 0, nl = 0;
  for (int l = 0; l < 4; l++)
    if (hud[l][0]) {
      u32 lw = hud_width(hud[l], 1);
      bw = lw > bw ? lw : bw, nl++;
    }
  if (!nl)
    return;
  u32 y = 218 - 10 * nl;
  hud_box(8, y - 5, bw + 16, 10 * nl + 7, HUD_PANEL);
  static const u32 col[4] = {HUD_GREY, HUD_GREY, HUD_TEXT, HUD_TEXT};
  for (int l = 0; l < 4; l++)
    if (hud[l][0]) {
      hud_text(16, y, hud[l], col[l], 1);
      y += 10;
    }
}

/* The copies of the scene (lod_sel). */
static void draw_copies(void) {
  float step = 2.4f * R, o = -step * itof(V.copies - 1) * 0.5f;
  for (int z = 0; z < V.copies; z++)
    for (int x = 0; x < V.copies; x++)
      draw_copy(o + step * itof(x), o + step * itof(z), lod_sel[z * V.copies + x]);
}

/* The scene drawn tile by tile into the target and blended into the frame's
   buffer buf; draw: the scene once (its copies). */
static void ss_draw(int buf, void (*draw)(void)) {
  struct mx_rt fb = mx_gs.rt[buf];
  fb.zpsm = MX_RT_NOZ, fb.zmsk = 1; /* the target is where Z was */
  /* the sprites draw with context 2: the frame's buffer */
  u64 *c = mx_frame_direct(5 + 5);
  c = mx_rt_e_env(c, &fb, 1, 0);
  c = mx_e_ad_tag(c, 4, 1);
  c = mx_e_ad(c, MX_GS_REG_TEX0_2, mx_rt_tex0(&ss_rt, 0, 1));
  c = mx_e_ad(c, MX_GS_REG_TEX1_2, mx_gs_tex1(0, 0, 1, 1, 0, 0, 0));
  c = mx_e_ad(c, MX_GS_REG_CLAMP_2, mx_gs_clamp(1, 1, 0, 0, 0, 0));
  c = mx_e_ad(c, MX_GS_REG_TEST_2, mx_gs_test(0, 0, 0, 0, 0, 0, 1, 1));
  mx_frame_commit(c);
  for (u32 ty = 0; ty < ss_tiles[1]; ty++)
    for (u32 tx = 0; tx < ss_tiles[0]; tx++) {
      u32 b[4];
      float cc[2], e[2];
      s32 off[2];
      ss_tile(tx, ty, b, cc, off, e);
      ss_cur = (int)(ty * ss_tiles[0] + tx);
      /* the texel at u covers u - 0.5 .. u + 0.5 (16ths) */
      float h = ss_s * 0.5f;
      u32 u0 = (u32)((SS_W / 2 + ss_s * (itof((s32)b[0]) - cc[0]) + h + e[0]) * 16 + 0.5f);
      u32 v0 = (u32)((SS_H / 2 + ss_s * (itof((s32)b[1]) - cc[1]) + h + e[1]) * 16 + 0.5f);
      u32 u1 = (u32)((SS_W / 2 + ss_s * (itof((s32)b[2]) - cc[0]) + h + e[0]) * 16 + 0.5f);
      u32 v1 = (u32)((SS_H / 2 + ss_s * (itof((s32)b[3]) - cc[1]) + h + e[1]) * 16 + 0.5f);
      for (u32 p = 0; p < (u32)ss_j; p++) {
        struct mx_rt t = ss_rt;
        s32 dx, dy;
        ss_jitter(p, (u32)ss_j, &dx, &dy);
        t.ox = (u32)((s32)t.ox + off[0] + dx), t.oy = (u32)((s32)t.oy + off[1] + dy);
        mx_frame_target(&t, 0);
        c = mx_frame_direct(2 + 5 + 2);
        c = mx_e_ad_tag(c, 1, 0);
        c = mx_e_ad(c, MX_GS_REG_TEST_1, mx_gs_test(0, 0, 0, 0, 0, 0, 1, 1));
        c = mx_rt_e_clear(c, &t, 0, 36, 40, 52, 0);
        c = mx_e_ad_tag(c, 1, 1);
        c = mx_e_ad(c, MX_GS_REG_TEST_1, mx_gs_test(1, 6, 0x40, 0, 0, 0, 1, 2));
        mx_frame_commit(c);
        cur_mat = ~0u;
        draw();
        mx_frame_texflush();
        /* pass p: (Cs - Cd) / (p + 1) + Cd, the mean of the passes */
        c = mx_frame_direct(7);
        c = mx_e_ad_tag(c, 6, 1);
        c = mx_e_ad(c, MX_GS_REG_ALPHA_2, mx_gs_alpha(0, 1, 2, 1, (128 + (p + 1) / 2) / (p + 1)));
        c = mx_e_ad(c, MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 1, 0, p != 0, 0, 1, 1, 0));
        c = mx_e_ad(c, MX_GS_REG_UV, mx_gs_uv(u0, v0));
        c = mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(mx_rt_px(&fb, b[0]), mx_rt_py(&fb, b[1]), 0));
        c = mx_e_ad(c, MX_GS_REG_UV, mx_gs_uv(u1, v1));
        c = mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(mx_rt_px(&fb, b[2]), mx_rt_py(&fb, b[3]), 0));
        mx_frame_commit(c);
      }
    }
  cur_mat = ~0u, ss_cur = -1;
  /* the HUD into the frame's buffer, without Z */
  mx_frame_target(&fb, 0);
}

/* One frame of the view at time t (seconds). */
static void frame(float t) {
  V.dist = clampf(V.dist, min_dist(), 20 * grid_radius());
  camera();
  ss_fit();
  st_frame.verts = st_frame.indices = st_frame.tris = st_frame.draws = st_frame.direct = 0;
  int buf = mx_frame_begin(36, 40, 52);
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
  float step = 2.4f * R, o = -step * itof(V.copies - 1) * 0.5f;
  for (u32 mt = 0; mt < M.h->nmat; mt++)
    lod_need[mt] = 0;
  for (int z = 0; z < V.copies; z++)
    for (int x = 0; x < V.copies; x++) {
      float mvp[4][4], light[3], eye[3];
      mvp_of(mvp, o + step * itof(x), o + step * itof(z), light, eye);
      lod_choose(mvp, lod_sel[z * V.copies + x]);
    }
  pose_frame(t, af);
  if (ss_j && ss_cull && ix_now())
    ss_bin();
  if (ss_j)
    ss_draw(buf, draw_copies);
  else
    draw_copies();
  pose_end();
  mx_frame_commit(mx_e_cnt(mx_frame_reserve(1), 0, mx_vif_flush(), 0));
  if (!no_hud)
    hud_draw();
  mx_frame_end();
}

/* ---- the pad ------------------------------------------------------------------------------- */

static u32 pad_pressed(void) { return mx_pad_pressed(0); }

/* ---- the run ------------------------------------------------------------------------------- */

static u8 ring[64 * 1024] MX_ALIGNED(64 * 1024);
#define FRAME_SPR ((void *)0x70002000)

/* the ring: the MFIFO's, or the slots of chain mode */
static void pipeline_restart(void) {
  if (use_mfifo)
    mx_frame_init(ring, sizeof ring, FRAME_SPR, 0x2000);
  else
    mx_frame_init_chain(ring, sizeof ring, FRAME_SPR, 0x2000);
  mx_vu_forget(1);
  bound = 0;
}

/* MIPMAP mode m (MIP_COLOURS skipped without the room for its levels);
   the level colours in or out of their host's place (probe_place) between
   frames: the frames drawn first, the pipeline restarted after. */
static void mip_set(int m) {
  if (m == MIP_COLOURS && probe_host == -1)
    m = (m + 1) % MIP_N;
  int swap = probe_host >= 0 && (m == MIP_COLOURS) != (mip_mode == MIP_COLOURS);
  mip_mode = m;
  if (!swap)
    return;
  u32 last = mx_gs.frames - 1, d0 = mx_count();
  while (mx_gs.frames_done < last + 1 && mx_count() - d0 < 300000000u)
    ;
  mx_frame_stop();
  if (m == MIP_COLOURS)
    probe_upload();
  else
    tex_upload((u32)probe_host);
  pipeline_restart();
  mx_frame_vsync(V.vsync);
}

/* The HUD's lines: the mode (hud_mode), the counts of the frame (hud_counts)
   and the speed (the interactive mode, the bench); drawn by hud_draw with
   the model, its outfit and animation. */
static void hud_mode(char *p) {
  p = hud_cat(p, shader_names[V.shader]);
  p = hud_cat(p, ix_now() ? "  INDEXED" : "  STRIPS");
  if (V.copies > 1) {
    p = hud_cat(p, "  ");
    p = hud_u(p, (u32)(V.copies * V.copies));
    p = hud_cat(p, " COPIES");
  }
  if (P.active)
    p = hud_cat(p, "  PHYS");
  if (aa1)
    p = hud_cat(p, "  AA1");
  if (mx_gs.smooth)
    p = hud_cat(p, mx_gs.smooth == MX_SMOOTH_DOWN ? "  DEFLICKER" : "  SOFT");
  if (ss_j) {
    /* the samples a pixel */
    p = hud_cat(p, "  SS ");
    p = hud_u(p, (u32)(ss_s * ss_s * itof(ss_j) + 0.5f));
  }
  if (V.shader >= S_TEX && mip_mode != MIP_OFF) {
    p = hud_cat(p, mip_names[mip_mode]);
    if (mip_bias) {
      /* in eighths of a level: +4/8 */
      *p++ = mip_bias < 0 ? '-' : '+', *p = 0;
      p = hud_u(p, (u32)(mip_bias < 0 ? -mip_bias : mip_bias) / 2);
      p = hud_cat(p, "/8");
    }
  }
  if (nlod > 1) {
    /* the level of each material (of the first copy) */
    p = hud_cat(p, "  LOD ");
    for (u32 mt = 0; mt < M.h->nmat; mt++)
      *p++ = (char)('0' + lod_sel[0][mt]), *p = 0;
  }
  if (animated(V.shader))
    hud_cat(p, "  EE SKIN");
}
static void hud_counts(char *p) {
  p = hud_u(p, st_frame.tris);
  p = hud_cat(p, " TRIANGLES  ");
  p = hud_u(p, st_frame.verts);
  hud_cat(p, " VERTICES");
}

/* The busy shares of the sampler (since mx_trace_sample was started). */
static void busy(u32 *vu1, u32 *path, u32 *gs) {
  u32 b[MX_TS_N], n = mx_trace_busy(b);
  *vu1 = n ? b[6] * 100 / n : 0, *path = n ? b[7] * 100 / n : 0, *gs = n ? b[8] * 100 / n : 0;
}

struct result {
  u32 shader, indexed, frames, tris, verts, indices, vu1, path, gs, ee;
  u64 cycles; /* a slow run is longer than the 32-bit Count's 14.6 s */
  u32 skin, skinned; /* a frame (on average) */
  u32 qw;            /* quadwords of frame stream a frame */
  u32 poses;         /* poses made */
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
  mx_printf("model bench %-6s %-7s: frame stream %u quadwords a frame (%s)\n",
            shader_names[r->shader], r->indexed ? "indexed" : "strips", r->qw,
            use_mfifo ? "MFIFO, batches per copy" : "mesh chains");
  if (r->skinned)
    mx_printf("model bench %-6s %-7s: EE skin %u.%u ms a frame, %u vertices, %u cycles a vertex, "
              "%u.%u poses/s\n",
              shader_names[r->shader], r->indexed ? "indexed" : "strips",
              (u32)((u64)r->skin * 10000 / MX_EE_HZ / 10),
              (u32)((u64)r->skin * 10000 / MX_EE_HZ % 10), r->skinned, r->skin / r->skinned,
              (u32)((u64)r->poses * MX_EE_HZ * 10 / r->cycles / 10),
              (u32)((u64)r->poses * MX_EE_HZ * 10 / r->cycles % 10));
}

static int want_interactive;

/* frames of the view as it is, without vsync: the counts a second */
static void bench(int shader, int indexed) {
  V.shader = shader, V.indexed = indexed;
  mx_frame_vsync(0);
  u32 waits0 = mx_frame_stats.wait_buffer + mx_frame_stats.wait_ring;
  mx_trace_sample(1000);
  u32 last = mx_count(), f0 = mx_gs.frames, v0 = mx_gs.vsync;
  u64 cycles = 0, sc0 = skin_cyc;
  u32 sn0 = skin_nv, p0 = J.poses, q0 = mx_frame_stats.qw;
  for (u32 f = 0; f < BENCH_FRAMES; f++) {
    if (pad_pressed() & MX_PAD_START)
      want_interactive = 1;
    frame(itof((s32)(mx_gs.vsync - v0)) / itof((s32)mx_gs.hz));
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
  r->skin = (u32)((skin_cyc - sc0) / BENCH_FRAMES), r->skinned = (skin_nv - sn0) / BENCH_FRAMES;
  /* a frame of its own pose each: -pose 1 or skin_frame */
  r->poses = J.poses - p0;
  r->qw = (mx_frame_stats.qw - q0) / BENCH_FRAMES;
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

/* The interactive view as it starts: a little nearer than all of the model
   fits the screen, on its upper half. */
static void view_start(void) {
  V.yaw = 0.6f, V.pitch = 0.2f, V.look = 0.25f * R, V.spin = 0;
  V.dist = 0.8f * fit_dist();
  camera();
}

/* One still frame of each shader as strips and indexed: the same picture. */
static int same_pictures(int nshader) {
  int fail = 0;
  if (!has_layout(0) || !has_layout(1)) {
    mx_printf("model: only %s, no pictures to compare\n", has_layout(1) ? "indexed" : "strips");
    return 0;
  }
  mx_frame_vsync(1);
  int pf = pose_frames;
  pose_frames = 1; /* each picture its own pose */
  V.copies = copies(2), V.spin = 0.7f, V.dist = min_dist();
  _Static_assert(S_SCEIC == S_SCEI + 1, "msceic is compared with the mscei strips before it");
  for (int s = 0; s < nshader; s++) {
    /* msceic: its strips (in line_hash[1]) against mscei's (line_hash[0]) */
    for (int ix = !has_ix(s); ix < 2; ix++) {
      V.shader = s, V.indexed = ix;
      hud[0][0] = hud[1][0] = hud[2][0] = hud[3][0] = 0;
      frame(0.25f);
      if (picture(line_hash[ix]) < 0) {
        mx_printf("model: the picture could not be read\n");
        pose_frames = pf;
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
  pose_frames = pf;
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
  /* the EE skins it for the programs: the bind pose of both layouts there,
     room for it, the joint
     matrices of the animations aligned for LQC2 */
  u32 most = skinned ? skin_most() : 0;
  M.ee_skin = skinned && most <= MAX_SKIN;
  for (int s = MXM_RPOS; s <= MXM_WGT; s++)
    M.ee_skin &= !M.h->nvert || M.h->off[s];
  for (int s = MXM_SRPOS; s <= MXM_SWGT; s++)
    M.ee_skin &= !M.h->nsvert || M.h->off[s];
  for (u32 a = 0; a < M.h->nanim; a++)
    M.ee_skin &= M.anim[a].mat % 16 == 0;
  if (skinned && !M.ee_skin) {
    mx_printf("model: the EE cannot skin it (%u vertices a pose, %u room)\n", most, MAX_SKIN);
    return -1;
  }
  J.on = J.front_ok = 0;
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
  V.copies = copies(V.copies);
  phys_open();
  hud_init(M.tbp_free, SH);
  return S_N;
}

static void interactive(int nshader) {
  mx_printf("model: interactive (SELECT + START ends)\n");
  mx_frame_vsync(V.vsync = 0); /* as fast as it draws (R2: vsync) */
  V.copies = 1, V.turning = 1, V.shader = S_SCEI;
  /* the dress pose, if it has one */
  for (u32 a = 0; a < M.h->nanim; a++)
    if (!strcmp_(M.anim[a].name, "DressPose"))
      V.anim = (int)a, P.reset = 1;
  view_start();
  hud_keys = 1;
  hud[0][0] = hud[1][0] = hud[2][0] = hud[3][0] = 0;
  u32 v0 = mx_gs.vsync, vp = v0, n = 0, tp = mx_count(), fp = mx_gs.frames;
  u32 wp = mx_frame_stats.wait_buffer + mx_frame_stats.wait_ring, ip = mx_frame_stats.idle;
  u64 tri = 0;
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
    if ((b & MX_PAD_CROSS) && has_layout(!V.indexed))
      V.indexed ^= 1;
    if ((b & MX_PAD_SELECT) && !(held & MX_PAD_START) && M.noutfit > 1 &&
        (outfit + 1 < M.noutfit || nmodel == 1)) {
      /* the next outfit: its clusters skinned anew */
      outfit = outfit + 1 < M.noutfit ? outfit + 1 : 0;
      J.on = J.front_ok = 0;
    } else if ((b & MX_PAD_SELECT) && !(held & MX_PAD_START) && nmodel > 1) {
      /* the textures change: once the frames before are drawn */
      u32 last = mx_gs.frames - 1, d0 = mx_count();
      while (mx_gs.frames_done < last + 1 && mx_count() - d0 < 300000000u)
        ;
      mx_frame_stop();
      pipeline_restart();
      mx_frame_vsync(V.vsync);
      u32 was = cur_model;
      outfit = 0;
      int ns = use_model((cur_model + 1) % nmodel);
      if (ns < 0)
        ns = use_model(was);
      nshader = ns;
    }
    if ((b & MX_PAD_TRIANGLE) && (M.h->flags & MXM_SINGLE)) {
      phys_off ^= 1;
      P.reset = 1;
    } else if (b & MX_PAD_TRIANGLE) {
      V.copies = V.copies >= 16 ? 1 : copies(V.copies * 2);
      view_start();
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
    if (b & MX_PAD_L3)
      view_start();
    if (b & MX_PAD_R3)
      quality_set((quality_at + 1) % (sizeof quality / sizeof *quality));
    if (b & MX_PAD_L1)
      mip_set((mip_mode + 1) % MIP_N);
    if (b & MX_PAD_R1)
      mip_bias = mip_bias >= 16 ? -16 : mip_bias + 8;
    u8 sticks[4];
    mx_pad_sticks(0, sticks);
    float ly = itof((s32)sticks[3] - 128) / 128;
    float rx = itof((s32)sticks[0] - 128) / 128, ry = itof((s32)sticks[1] - 128) / 128;
    ly = ly * ly < 0.02f ? 0 : ly;
    rx = rx * rx < 0.02f ? 0 : rx, ry = ry * ry < 0.02f ? 0 : ry;
    /* the D-pad (a digital pad): turn the model left and right, zoom */
    if (held & MX_PAD_LEFT)
      rx = -1;
    if (held & MX_PAD_RIGHT)
      rx = 1;
    if (held & MX_PAD_UP)
      ly = -1;
    if (held & MX_PAD_DOWN)
      ly = 1;
    /* the right stick turns the model (and stops its turning by itself)
       and tilts the view; the left one zooms (up: nearer) */
    float t = itof((s32)(mx_gs.vsync - v0)) / itof((s32)mx_gs.hz);
    float dt = itof((s32)(mx_gs.vsync - vp)) / itof((s32)mx_gs.hz);
    vp = mx_gs.vsync;
    if (rx != 0)
      V.turning = 0;
    V.spin += rx * 0.045f + (V.turning ? 0.15f * dt : 0);
    V.pitch = clampf(V.pitch + ry * 0.03f, -1.4f, 1.4f);
    V.dist *= 1 + ly * 0.03f;
    frame(t);
    tri += st_frame.tris, n++;
    hud_mode(hud[0]);
    hud_counts(hud[1]);
    if (n == 60) {
      u32 cyc = mx_count() - tp, fr = mx_gs.frames - fp, vu1, path, gs;
      busy(&vu1, &path, &gs);
      /* the EE: all but its waits for the GS and VU1 (the idle work in
         them, the skinning, counts); VU0 in micro mode its own, in macro
         mode (strips, -skin macro) a part of the EE's */
      u32 b[MX_TS_N], ns = mx_trace_busy(b);
      u32 wt = mx_frame_stats.wait_buffer + mx_frame_stats.wait_ring - wp;
      u32 idle = mx_frame_stats.idle - ip, w = wt > idle ? wt - idle : 0;
      u32 ee = w < cyc ? (u32)((u64)(cyc - w) * 100 / cyc) : 0;
      int micro = M.skinned && animated(V.shader) && ix_now() && skin_micro;
      char *p = hud[2];
      p = hud_u(p, (u32)((u64)fr * MX_EE_HZ / cyc));
      p = hud_cat(p, V.vsync ? " FPS  " : " FPS NO VSYNC  ");
      p = hud_u1(p, tri * MX_EE_HZ / cyc, 1000000);
      hud_cat(p, "M TRIANGLES/S");
      p = hud_cat(hud[3], micro ? "EE " : "EE/VU0 ");
      p = hud_cat(hud_u(p, ee), "%  ");
      if (micro)
        p = hud_cat(hud_u(hud_cat(p, "VU0 "), ns ? b[5] * 100 / ns : 0), "%  ");
      p = hud_cat(hud_u(hud_cat(p, "VU1 "), vu1), "%  ");
      hud_cat(hud_u(hud_cat(p, "GS "), gs), "%");
      mx_trace_sample(0);
      mx_trace_sample(1000);
      tp = mx_count(), fp = mx_gs.frames, n = 0, tri = 0;
      wp = mx_frame_stats.wait_buffer + mx_frame_stats.wait_ring, ip = mx_frame_stats.idle;
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
  pose_frames = 1; /* each picture its own pose */
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

#ifdef MODEL_MIPSHOTS
/* -DMODEL_MIPSHOTS: still pictures of the start view's angle at 0.5, 1, 3,
   6 and 12 times fit_dist, in each MIPMAP mode (mip_mode), without and with
   supersampling (16 samples), mscei, without the HUD:
   host:mip_d<k>_<mode>[_ss16].tga. */
static void mip_shots(void) {
  static const char *const name[MIP_N] = {"off", "on", "tri", "col"};
  static const float dk[] = {0.5f, 1.0f, 3.0f, 6.0f, 12.0f};
  mx_frame_vsync(1);
  pose_frames = 1;
  no_hud = 1;
  V.copies = 1, V.shader = S_SCEI, V.indexed = has_layout(1);
  for (u32 k = 0; k < sizeof dk / sizeof *dk; k++)
    for (u32 ms = 0; ms < 2 * MIP_N; ms++) {
      int m = (int)(ms % MIP_N), ss = ms >= MIP_N;
      view_start();
      V.dist = dk[k] * fit_dist();
      mip_set(m);
      mx_frame_vsync(1);
      ss_set(ss ? 16 : 0);
      frame(0.5f);
      frame(0.5f);
      u32 last = mx_gs.frames - 1, d0 = mx_count();
      while (mx_gs.frames_done < last + 1 && mx_count() - d0 < 300000000u)
        ;
      mx_frame_stop();
      char path[40] = "host:mip_d";
      char *e = hud_u(path + 10, k);
      e = hud_cat(e, "_");
      e = hud_cat(e, name[m]);
      hud_cat(e, ss ? "_ss16.tga" : ".tga");
      if (mx_gs_save_tga((int)(last & 1), path) < 0)
        mx_printf("model: %s could not be written\n", path);
      else
        mx_printf("model: %s\n", path);
      pipeline_restart();
    }
}
#endif

#ifdef MODEL_SHOTS
/* -DMODEL_SHOTS: still pictures of the start view's angle at 0.8, 1.6 and
   3 times fit_dist, each without and with AA1 and supersampling, without
   the HUD: host:shot_d<k>_<mode>.tga. */
static void shots(void) {
  static const struct {
    const char *name;
    u8 aa1, ss;
  } mode[] = {{"off", 0, 0},   {"aa1", 1, 0},   {"ss4", 0, 4},     {"ss8", 0, 8},
              {"ss16", 0, 16}, {"ss32", 0, 32}, {"ss16aa1", 1, 16}};
  static const float dk[] = {0.8f, 1.6f, 3.0f};
  mx_frame_vsync(1);
  pose_frames = 1;
  no_hud = 1;
  V.copies = 1, V.shader = S_SCEI, V.indexed = has_layout(1);
  for (u32 k = 0; k < sizeof dk / sizeof *dk; k++)
    for (u32 m = 0; m < sizeof mode / sizeof *mode; m++) {
      view_start();
      V.dist = dk[k] * fit_dist();
      aa1 = mode[m].aa1;
      ss_set(mode[m].ss);
      frame(0.5f);
      frame(0.5f);
      u32 last = mx_gs.frames - 1, d0 = mx_count();
      while (mx_gs.frames_done < last + 1 && mx_count() - d0 < 300000000u)
        ;
      mx_frame_stop();
      char name[40] = "host:shot_d";
      char *e = hud_u(name + 11, k);
      e = hud_cat(e, "_");
      e = hud_cat(e, mode[m].name);
      hud_cat(e, ".tga");
      mx_printf("model: %s: %u tiles of %u passes, scale %u%%\n", name, ss_tiles[0] * ss_tiles[1],
                (u32)ss_j, (u32)(ss_s * 100 + 0.5f));
      if (mx_gs_save_tga((int)(last & 1), name) < 0)
        mx_printf("model: %s could not be written\n", name);
      pipeline_restart();
      /* the tiles' clusters: the same picture as with all of them drawn */
      if (mode[m].ss) {
        u32 nv[2], dr[2], diff = 0;
        for (int c = 0; c < 2; c++) {
          ss_cull = !c;
          frame(0.5f);
          frame(0.5f);
          nv[c] = st_frame.verts, dr[c] = st_frame.direct;
          if (picture(line_hash[c]) < 0)
            mx_printf("model: %s: no picture\n", name);
        }
        ss_cull = 1;
        for (u32 y = 0; y < SH; y++)
          diff += line_hash[0][y] != line_hash[1][y];
        mx_printf("model: %s: culled %u vertices drawn, all %u; %u lines differ; %u, %u draws "
                  "without a chain\n",
                  name, nv[0], nv[1], diff, dr[0], dr[1]);
      }
      /* the time a frame takes, without vsync */
      mx_frame_vsync(0);
      frame(0.5f);
      u32 t0 = mx_count();
      for (int i = 0; i < 60; i++)
        frame(0.5f);
      last = mx_gs.frames - 1;
      while (mx_gs.frames_done < last + 1)
        ;
      u32 us = (mx_count() - t0) / 60 / 295;
      mx_printf("model: %s: %u.%02u ms a frame\n", name, us / 1000, us % 1000 / 10);
      mx_frame_vsync(1);
    }
}
#endif

int main(int argc, char **argv) {
  mx_gs_init(SW, SH);
  mx_vu_reset(3);
  mx_vu_load(0, vu0skin_vu, (u32)(vu0skin_vu_end - vu0skin_vu), 0);
  pipeline_restart();
  mx_frame_idle(pose_idle);
  sh[S_FLAT][0] = MX_VU_SHADER(MFLAT, mflat_vu, mflat_vu_end);
  sh[S_FLAT][1] = MX_VU_ISHADER(IX_MFLAT, mflat_ix_vu, mflat_ix_vu_end);
  sh[S_LIT][0] = MX_VU_SHADER(MLIT, mlit_vu, mlit_vu_end);
  sh[S_LIT][1] = MX_VU_ISHADER(IX_MLIT, mlit_ix_vu, mlit_ix_vu_end);
  sh[S_TEX][0] = MX_VU_SHADER(MTEX, mtex_vu, mtex_vu_end);
  sh[S_TEX][1] = MX_VU_ISHADER(IX_MTEX, mtex_ix_vu, mtex_ix_vu_end);
  sh[S_SCEI][0] = MX_VU_SHADER(MSCEI, mscei_vu, mscei_vu_end);
  sh[S_SCEI][1] = MX_VU_ISHADER(IX_MSCEI, mscei_ix_vu, mscei_ix_vu_end);
  sh[S_SCEIC][0] = MX_VU_SHADER(MSCEIC, msceic_vu, msceic_vu_end);
  /* the position V4-32 (sh_ee: skin_pos); in strips with its w, the restart
     flag, else w 1.0 from the VIF (the program's STMASK) */
  for (int s = 0; s < S_N; s++)
    for (int k = 0; k < 2; k++) {
      if (!k)
        sh[s][k].in[0].fmt = MX_UNPACK_V4_32, sh[s][k].in[0].bytes = 16, sh[s][k].in[0].mask = 0;
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
    if (!strcmp_(argv[a], "-mfifo")) {
      use_mfifo = 1;
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
    if (!strcmp_(argv[a], "-outfit") && a + 1 < argc) {
      outfit = atou_(argv[++a]);
      continue;
    }
    if (!strcmp_(argv[a], "-lodr") && a + 1 < argc) {
      lod_r0 = itof((s32)atou_(argv[++a]));
      continue;
    }
    if (!strcmp_(argv[a], "-skin") && a + 1 < argc) {
      skin_micro = strcmp_(argv[++a], "macro") != 0;
      continue;
    }
    if (!strcmp_(argv[a], "-pose") && a + 1 < argc) {
      pose_frames = (int)atou_(argv[++a]);
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
    if (!strcmp_(argv[a], "-mip") && a + 1 < argc) {
      static const char *const mode[MIP_N] = {"off", "on", "tri", "col"};
      a++;
      for (int i = 0; i < MIP_N; i++)
        if (!strcmp_(argv[a], mode[i]))
          mip_mode = i;
      continue;
    }
    if (!strcmp_(argv[a], "-mipbias") && a + 1 < argc) {
      const char *v = argv[++a];
      mip_bias = v[0] == '-' ? -(s32)atou_(v + 1) : (s32)atou_(v);
      continue;
    }
    if (!strcmp_(argv[a], "-aa")) {
      aa1 = 1;
      continue;
    }
    if (!strcmp_(argv[a], "-soft")) {
      mx_gs_smooth(MX_SMOOTH_ACROSS);
      continue;
    }
    if (!strcmp_(argv[a], "-deflicker")) {
      mx_gs_smooth(MX_SMOOTH_DOWN);
      continue;
    }
    if (!strcmp_(argv[a], "-nocull")) {
      ss_cull = 0;
      continue;
    }
    if (!strcmp_(argv[a], "-ss")) {
      u32 n = a + 1 < argc && argv[a + 1][0] >= '0' && argv[a + 1][0] <= '9' ? atou_(argv[++a]) : 4;
      ss_set(n == 8 || n == 16 || n == 32 ? n : 4);
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
  if (use_mfifo) {
    mx_frame_stop();
    pipeline_restart();
  }
  if (!nmodel)
    model_add(model_fox, (u32)(model_fox_end - model_fox), "fox");
  mesh_room(file_buf + used, MAX_FILE - used);
  int nshader = use_model(0);
  if (nshader < 0) {
    MX_TEST_END(1);
    return 1;
  }
  if (mx_pad_init() < 0)
    mx_printf("model: no pad driver\n");
#ifdef MODEL_SHOTS
  shots();
  mx_frame_stop();
  MX_TEST_END(0);
  return 0;
#endif
#ifdef MODEL_MIPSHOTS
  mip_shots();
  mx_frame_stop();
  MX_TEST_END(0);
  return 0;
#endif
#ifdef MODEL_RECORD
  record();
  mx_frame_stop();
  MX_TEST_END(0);
  return 0;
#endif

  int fail = skin_check() != 0;
  fail |= want_interactive ? 0 : same_pictures(nshader);

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
