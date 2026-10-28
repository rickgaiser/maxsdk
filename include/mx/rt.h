#ifndef MX_RT_H
#define MX_RT_H

/* Render targets: a framebuffer (and Z buffer) in GS memory that a GS
   drawing context draws into, and that can be read back as a texture or
   shown by a read circuit (<mx/display.h>).

   The GS has two drawing contexts: FRAME, ZBUF, XYOFFSET, SCISSOR, TEST,
   ALPHA, TEX0/1/2, CLAMP, MIPTBP1/2 and FBA exist twice, the second at the
   first's address + 1 (mx_gs_ctx). PRIM.CTXT (mx_gs_prim's ctxt) picks the
   context of each primitive. With a frame buffer in one context and a
   texture target in the other, switching between them costs nothing (the GS
   draws in order); only a TEXFLUSH is needed before the target is read. */

#include <mx/emit.h>
#include <mx/gif.h>
#include <mx/types.h>

/* The register of context ctx (0, 1): reg is the context 1 register
   (MX_GS_REG_FRAME_1, ...). */
MX_INLINE u32 mx_gs_ctx(u32 reg, int ctx) { return reg + (u32)ctx; }

#define MX_RT_NOZ 0 /* zpsm: no Z buffer (ZBUF.ZMSK = 1) */

struct mx_rt {
  u32 fbp, fbw, psm;      /* FRAME: page (8 KiB), width in 64-pixel units, PSM (CT32/24/16/16S) */
  u32 w, h;               /* size in pixels */
  u32 fbmsk;              /* FRAME.FBMSK: bits not written */
  u32 zbp, zpsm, zmsk;    /* ZBUF: page, PSM (Z32/24/16/16S, or MX_RT_NOZ), no Z writes */
  u32 ox, oy;             /* XYOFFSET (12.4): the target's window origin in primitive coordinates */
  u16 sx0, sx1, sy0, sy1; /* SCISSOR, in window pixels */
};

/* A target of w x h pixels at page fbp with the Z buffer at page zbp (zpsm
   MX_RT_NOZ: none). Window (0, 0) is at primitive (2048 - w/2, 2048 - h/2);
   the scissor is the whole target. */
void mx_rt_init(struct mx_rt *t, u32 fbp, u32 w, u32 h, u32 psm, u32 zbp, u32 zpsm);
/* 8 KiB pages of a w x h buffer of format psm (a page is 64 x 32 pixels for
   32/24-bit formats, 64 x 64 for 16-bit, 128 x 64 for T8, 128 x 128 for T4). */
u32 mx_rt_pages(u32 w, u32 h, u32 psm);

/* A+D writes (no GIF tag) of FRAME, ZBUF, XYOFFSET and SCISSOR of context
   ctx: 4 quadwords. */
u64 *mx_rt_e_regs(u64 *c, const struct mx_rt *t, int ctx);
/* The same with its GIF tag: 5 quadwords. */
u64 *mx_rt_e_env(u64 *c, const struct mx_rt *t, int ctx, int eop);
/* Clear the whole target to (r, g, b) and Z to 0 with a sprite of context
   ctx (TEST is not changed: Z is written if ZTST passes): 5 quadwords. */
u64 *mx_rt_e_clear(u64 *c, const struct mx_rt *t, int ctx, u32 r, u32 g, u32 b, int eop);

/* TEX0 to read the target as a texture: TBP0 = the target's page, TBW =
   FBW, its PSM, TW/TH = the smallest powers of two that hold it. 0 if the
   target is wider or higher than 1024 pixels (a texture cannot be). */
u64 mx_rt_tex0(const struct mx_rt *t, u32 tcc, u32 tfx);

/* Window pixel coordinates of t to primitive coordinates (12.4). */
MX_INLINE u32 mx_rt_px(const struct mx_rt *t, u32 x) { return t->ox + (x << 4); }
MX_INLINE u32 mx_rt_py(const struct mx_rt *t, u32 y) { return t->oy + (y << 4); }

#endif
