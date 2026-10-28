#ifndef MX_TEX_H
#define MX_TEX_H

/* Textures in GS memory with MIPMAP levels: where the levels are, the
   register values that read them in either context, their upload, the LOD
   parameter K, and levels made by the GS from the level above (render
   targets).

   Level 0 is (1 << tw) x (1 << th) texels, level n half of level n - 1 each
   way; TEX0 addresses level 0, MIPTBP1 levels 1-3 and MIPTBP2 levels 4-6,
   each with its own base and buffer width. TEX1.MXL is the last level used.
   The level is picked per pixel by LOD = (log2(1/Q) << L) + K (TEX1.LCM 0,
   Q interpolated: STQ only), or LOD = K (LCM 1); LOD <= 0 uses level 0
   and the MMAG filter, above that MMIN. The GS knows the distance (Q), not
   the slope of the surface: L > 0 makes the level rise faster with the
   distance, for surfaces seen at a slant. */

#include <mx/emit.h>
#include <mx/gif.h>
#include <mx/pkt.h>
#include <mx/rt.h>
#include <mx/types.h>

#define MX_TEX_LEVELS 7

/* layout of mx_tex_init: levels packed (sub-page levels share pages, the
   way of the GS manual's example), or each level from a page of its own
   (needed to draw into the levels: mx_tex_rt, mx_tex_e_mipgen) */
enum { MX_TEX_PACKED = 0, MX_TEX_PAGES = 1 };

struct mx_tex {
  u32 tbp[MX_TEX_LEVELS]; /* 64-word block of each level */
  u8 tbw[MX_TEX_LEVELS];  /* buffer width of each level (64 pixels) */
  u8 psm, tw, th, levels; /* levels 1 .. MX_TEX_LEVELS */
  u32 cbp, cpsm, csm;     /* CLUT of T8/T4 (all levels share it); set after mx_tex_init */
  u32 blocks;             /* blocks from tbp[0] that the levels take */
};

/* Page size of format psm in pixels (64 x 32 for 32/24-bit formats and
   T8H/T4HL/T4HH, 64 x 64 for 16-bit, 128 x 64 for T8, 128 x 128 for T4). */
void mx_tex_page_size(u32 psm, u32 *pw, u32 *ph);
/* The least buffer width (64 pixels) of a w pixel wide texture: a page
   wide at least (2 for T8/T4). */
u32 mx_tex_tbw(u32 w, u32 psm);
/* The blocks from its base that a w x h texture of psm touches, at width
   mx_tex_tbw: whole pages when it is wider or higher than a page, else up to the
   last block of the page's block order that it covers. */
u32 mx_tex_blocks(u32 w, u32 h, u32 psm);
/* The levels of a (1 << tw) x (1 << th) texture down to min x min texels
   (a power of two: 8 for bilinear filters, which are wrong at 4 texels or
   less; also the least IMAGE transfer width of T8/T4), at most
   MX_TEX_LEVELS. */
u32 mx_tex_max_levels(u32 tw, u32 th, u32 min);
/* Place the levels of a texture from block tbp (a page for MX_TEX_PAGES):
   largest first; a level of a page or more from a page, a smaller one
   aligned to its block count rounded up to a power of two. Returns
   t->blocks. The CLUT is unset (cbp 0, CT32, CSM1). */
u32 mx_tex_init(struct mx_tex *t, u32 tbp, u32 psm, u32 tw, u32 th, u32 levels, int layout);

/* TEX0 of level 0 (with the CLUT of T8/T4 and CLD 1: loaded when TEX0 is
   written). */
u64 mx_tex_tex0(const struct mx_tex *t, u32 tcc, u32 tfx);
/* MIPTBP1 (levels 1-3), MIPTBP2 (levels 4-6); missing levels 0. */
u64 mx_tex_miptbp1(const struct mx_tex *t);
u64 mx_tex_miptbp2(const struct mx_tex *t);
/* TEX1: LCM 0, MXL the last level, filters MMAG (0 nearest, 1 linear)
   and MMIN (GS TEX1.MMIN: 0 nearest, 1 linear, 2/3 nearest and the
   nearest/both levels, 4/5 linear and the nearest/both levels), L and K
   (1/16, from mx_tex_lod_k). */
u64 mx_tex_tex1(const struct mx_tex *t, u32 mmag, u32 mmin, u32 l, s32 k);
/* A+D writes (no GIF tag) of TEX1, TEX0, MIPTBP1, MIPTBP2 of context ctx:
   4 quadwords. */
u64 *mx_tex_e_regs(u64 *c, const struct mx_tex *t, int ctx, u64 tex0, u64 tex1);

/* Host -> local transfer of a level (mx_gs_upload, then TEXFLUSH). */
void mx_tex_upload(struct mx_pkt *p, const struct mx_tex *t, u32 level, const void *data);

/* K (1/16, clamped to TEX1.K) that puts level 0 where a texel is a pixel:
   texels_per_unit of the texture on the surface (model units), and
   pixels_per_unit of the screen at w = 1 (Q = 1/w: the projection's scale
   in pixels). LOD = log2(texels a pixel) << l. Add a bias to it (-8 is half
   a level sharper). */
s32 mx_tex_lod_k(float texels_per_unit, float pixels_per_unit, u32 l);

/* A render target of one level (MX_TEX_PAGES; CT32/24/16/16S), no Z. */
void mx_tex_rt(const struct mx_tex *t, u32 level, struct mx_rt *rt);
/* The GS makes levels 1 .. levels - 1, each the mean of 2 x 2 texels of
   the level above (a bilinear sprite of context ctx half its size). For
   textures of MX_TEX_PAGES that were drawn into. Changes FRAME, ZBUF,
   XYOFFSET, SCISSOR, TEST, TEX0, TEX1, CLAMP of ctx: set the target again
   after (mx_frame_target). A GIF tag of its own per level, eop on the last:
   MX_TEX_MIPGEN_QW quadwords. */
#define MX_TEX_MIPGEN_QW(levels) (15 * ((levels) - 1))
u64 *mx_tex_e_mipgen(u64 *c, const struct mx_tex *t, int ctx, int eop);

#endif
