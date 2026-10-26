/* MXM: a model for VU1 programs built from HLSL, made by
   tools/model/gltf2mxm.py from glTF. Little endian; a header, then sections
   at offsets that are multiples of 64 bytes, so the file read to a 64-byte
   aligned address can be sent by DMA as it is.

   The vertex streams are in the formats of the shader inputs
   (demos/model/m*.hlsl): position V3-32 (12 bytes), normal V4-8 (4),
   texture coordinates V2-16 4.12 (4), joints V4-8U (palette slots) and
   weights V4-8U (sum 255, the largest first; a joint of weight 0 is one of
   the palette too). A skinned model has its positions and normals
   only as the skin has them (RPOS, RNRM; no POS, NRM): its rest pose is
   the first animation. RPOS is V3-16 (6 bytes): 1 / 32768 of qsize from
   qoff, so its clusters and strip runs start at multiples of 8 vertices
   (where the stream is quadword aligned) and 7 more vertices may be read
   after the last.

   Indexed: each cluster is one batch, nv vertices from vertex first of the
   streams (a multiple of 4, of 8 skinned; the batch reads up to 3, 7 more)
   and ni indices at idx in IDX (the vertex numbers in the cluster: times
   the program's OUT_QW before they are sent; 16-byte aligned, padded to
   16), a triangle strip whose pieces are joined by repeated vertices;
   skinned, its vertices of one joint (weight 255) come first, then those
   of two, then the rest. Strips: the same strips, the vertices written out
   (the S* streams), one run per material and palette.
   A model may have only one of the two (gltf2mxm.py --layout): the other's
   sections are left out and its counts (ncluster and nvert, or nstrip and
   nsvert) are 0.

   Levels of detail: a cluster's or strip run's mat is the material in its
   low byte and the level in its high one (0: the most detail; level k has
   about 2^-k of the triangles of each part). The clusters and runs of a
   level come together, its materials one after the other; ntri counts all
   levels.

   A skinned model's animations are baked: for each frame, the matrix of
   every joint (its node's world matrix times its inverse bind matrix) as
   four columns; the first animation is the rest pose. */
#ifndef DEMOS_MXM_H
#define DEMOS_MXM_H

#include <mx/types.h>

#define MXM_MAGIC 0x334D584Du /* "MXM3" */
#define MXM_SKINNED 1u
#define MXM_SINGLE 2u /* drawn once, not as copies (gltf2mxm.py --single) */

enum {
  MXM_POS, MXM_NRM, MXM_UV, MXM_RPOS, MXM_RNRM, MXM_JNT, MXM_WGT, MXM_IDX, MXM_CLUSTER,
  MXM_SPOS, MXM_SNRM, MXM_SUV, MXM_SRPOS, MXM_SRNRM, MXM_SJNT, MXM_SWGT, MXM_STRIP,
  MXM_MAT, MXM_TEX, MXM_PAL, MXM_ANIM, MXM_PHYS, MXM_COLL,
  MXM_NSEC = 24
};

struct mxm_header {
  u32 magic, bytes, flags, nvert;  /* nvert: vertices of the cluster streams */
  u32 ncluster, ntri, nsvert, nstrip;
  u32 nmat, njoint, npal, nanim;
  u32 max_nv, max_ni, pal_joints, tex_bytes; /* the limits it was made for */
  float center[3], radius;         /* the bounds of the rest pose */
  float qoff[3], qsize;            /* RPOS: q / 32768 * qsize + qoff (skinned) */
  u32 off[MXM_NSEC];               /* section offsets, 0: none */
};

struct mxm_cluster {
  u32 first;
  u16 nv, ni;
  u32 idx;      /* byte offset in IDX */
  u16 mat, pal; /* mat: the material | the level << 8 */
  float sphere[4];
};

struct mxm_strip {
  u32 first, n; /* vertices of the S* streams */
  u16 mat, pal; /* mat: the material | the level << 8 */
  u32 pad;
};

/* The texture: psm MXM_TEX_CT32, (1 << tw) x (1 << th) RGBA (alpha 128 =
   1.0); or MXM_TEX_T8, as many bytes, each the number of an entry of the
   CLUT at clut: 256 RGBA, already in the GS's order of a 16 x 16 CT32 CLUT
   (CSM1: entries 8 .. 15 and 16 .. 23 of each 32 swapped). */
#define MXM_TEX_CT32 0
#define MXM_TEX_T8 1
#define MXM_MAT_ALPHA 1 /* flags: texels not fully opaque (cut-outs, soft edges) */
struct mxm_mat {
  u32 tex;      /* byte offset in TEX */
  u8 tw, th;
  u8 psm, flags;
  u8 color[4];  /* the texture's mean colour; [3]: the highlight's strength (0..255) */
  u32 clut;     /* byte offset in TEX (T8) */
};

#define MXM_PAL_MAX 30
struct mxm_pal {
  u16 n;
  u16 joint[MXM_PAL_MAX];
  u16 pad;
};

struct mxm_anim {
  char name[16];
  u32 nframe;
  float fps;
  u32 mat;      /* byte offset in ANIM: nframe * njoint matrices of 4 float4 columns */
  u32 pad;
};

/* PHYS: the joints moved by springs (gltf2mxm.py --phys), parents before
   children: a u32 count (the section's first 16 bytes), the records, then the
   u16 joint numbers of their descendants. The tip of each is pulled by a
   spring towards where the animation puts it (stiff per second squared; damp
   per second, of its speed relative to the animation's; plus gravity down,
   model units per second squared), kept at its distance from the head,
   within limit (radians) of the animation and out of the COLL capsules; the
   joint and its descendants turn about the head to it. */
struct mxm_phys {
  u16 joint, ndesc;
  u32 desc;             /* byte offset in PHYS of the ndesc descendants */
  float head[4], tip[4]; /* in the rest pose (model space), w 1 */
  float stiff, damp, gravity, limit;
  float radius;         /* its thickness: kept that far out of the capsules */
  u32 pad[3];
};
_Static_assert(sizeof(struct mxm_phys) == 72, "gltf2mxm.py writes 72 bytes");

/* COLL: capsules on body joints (gltf2mxm.py --collide) that the spring
   joints stay out of: a u32 count (the first 16 bytes), then the records.
   The segment a .. b is in the rest pose (model space) and moves with the
   joint. */
struct mxm_coll {
  u16 joint, pad;
  float radius;
  float a[3], b[3];
};
_Static_assert(sizeof(struct mxm_coll) == 32, "gltf2mxm.py writes 32 bytes");

#endif
