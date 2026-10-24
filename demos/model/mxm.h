/* MXM: a model for VU1 programs built from HLSL, made by
   tools/model/gltf2mxm.py from glTF. Little endian; a header, then sections
   at offsets that are multiples of 64 bytes, so the file read to a 64-byte
   aligned address can be sent by DMA as it is.

   The vertex streams are in the formats of the shader inputs
   (demos/model/m*.hlsl): position V3-32 (12 bytes), normal V4-8 (4),
   texture coordinates V2-16 4.12 (4), joints V4-8U (palette slots) and
   weights V4-8U (sum 255). A skinned model has the positions and normals
   twice: of the rest pose (POS, NRM) and as the skin has them (RPOS, RNRM).

   Indexed: each cluster is one batch, nv vertices from vertex first of the
   streams (a multiple of 4; the batch reads up to 3 more) and ni indices at
   idx in IDX (the vertex numbers in the cluster: times the program's OUT_QW
   before they are sent; 16-byte aligned, padded to 16), a triangle strip
   whose pieces are joined by repeated vertices. Strips: the same strips, the
   vertices written out (the S* streams), one run per material and palette.

   A skinned model's animations are baked: for each frame, the matrix of
   every joint (its node's world matrix times its inverse bind matrix) as
   four columns; the first animation is the rest pose. */
#ifndef DEMOS_MXM_H
#define DEMOS_MXM_H

#include <mx/types.h>

#define MXM_MAGIC 0x314D584Du /* "MXM1" */
#define MXM_SKINNED 1u

enum {
  MXM_POS, MXM_NRM, MXM_UV, MXM_RPOS, MXM_RNRM, MXM_JNT, MXM_WGT, MXM_IDX, MXM_CLUSTER,
  MXM_SPOS, MXM_SNRM, MXM_SUV, MXM_SRPOS, MXM_SRNRM, MXM_SJNT, MXM_SWGT, MXM_STRIP,
  MXM_MAT, MXM_TEX, MXM_PAL, MXM_ANIM,
  MXM_NSEC = 24
};

struct mxm_header {
  u32 magic, bytes, flags, nvert;  /* nvert: vertices of the cluster streams */
  u32 ncluster, ntri, nsvert, nstrip;
  u32 nmat, njoint, npal, nanim;
  u32 max_nv, max_ni, pal_joints, tex_bytes; /* the limits it was made for */
  float center[3], radius;         /* the bounds of the rest pose */
  u32 off[MXM_NSEC];               /* section offsets, 0: none */
};

struct mxm_cluster {
  u32 first;
  u16 nv, ni;
  u32 idx;      /* byte offset in IDX */
  u16 mat, pal;
  float sphere[4];
};

struct mxm_strip {
  u32 first, n; /* vertices of the S* streams */
  u16 mat, pal;
  u32 pad;
};

struct mxm_mat {
  u32 tex;      /* byte offset in TEX: (1 << tw) x (1 << th) RGBA, alpha 128 = 1.0 */
  u16 tw, th;
  u8 color[4];  /* the texture's mean colour */
  u32 pad;
};

#define MXM_PAL_MAX 30
struct mxm_pal {
  u16 n;
  u8 joint[MXM_PAL_MAX];
};

struct mxm_anim {
  char name[16];
  u32 nframe;
  float fps;
  u32 mat;      /* byte offset in ANIM: nframe * njoint matrices of 4 float4 columns */
  u32 pad;
};

#endif
