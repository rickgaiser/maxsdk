#ifndef MX_DISPLAY_H
#define MX_DISPLAY_H

/* What the PCRTC shows: two read circuits, each a rectangle of a buffer in
   GS memory (DISPFB) placed on the screen (DISPLAY), and the merge circuit
   (PMODE, BGCOLOR). Circuit 2 is the bottom layer (or BGCOLOR where it does
   not reach); circuit 1 is blended over it with the alpha of its own pixels
   (0x80 = 1.0) or a constant (GS manual 5.2.3):

     out = c1 * a + (c2 or BGCOLOR) * (1 - a)

   The circuits may show different buffers, formats, sizes and
   magnifications: a 640 x 448 HUD over 3D drawn at 320 x 224, a crossfade
   between two buffers, a fade to a colour.

   A display is plain data; mx_display_regs() turns it into the register
   values for a video mode (<mx/video.h>). The frame pipeline shows a list of
   displays, one per frame (mx_gs_displays). */

#include <mx/rt.h>
#include <mx/types.h>
#include <mx/video.h>

struct mx_circuit {
  u8 on;
  u32 fbp, fbw, psm; /* DISPFB: buffer page, width in 64-pixel units, PSM */
  u32 dbx, dby;      /* DISPFB: top left of the rectangle in the buffer */
  u32 w, h;          /* rectangle size in pixels */
  u32 magh, magv;    /* VCK per pixel (1-16), DISPLAY lines per buffer line at MAGV 0 times
                        this (1-4); 0: the largest that fits the mode's active area */
  s32 dx, dy;        /* move from the centre of the active area: VCK, DISPLAY lines */
};

struct mx_display {
  struct mx_circuit c[2]; /* read circuit 1 (blended on top), read circuit 2 */
  u8 mmod;                /* blend alpha: 0 circuit 1's pixels, 1 alp */
  u8 amod;                /* alpha output to the feedback write: 0 circuit 1, 1 circuit 2 */
  u8 slbg;                /* under circuit 1: 0 circuit 2, 1 BGCOLOR */
  u8 alp;                 /* constant alpha (0xFF = 1.0) */
  u32 bgcolor;            /* 0xBBGGRR (Sony: keep it black for TVs except for fades) */
};

/* Circuit c shows all of target t, fitted to the active area. */
void mx_circuit_rt(struct mx_circuit *c, const struct mx_rt *t);
/* d shows t on circuit 2 alone (the merge set up as sceGsSetDefDispEnv). */
void mx_display_init(struct mx_display *d, const struct mx_rt *t);

/* Register values of d in mode m: PMODE, DISPFB1, DISPLAY1, DISPFB2,
   DISPLAY2, BGCOLOR. */
enum {
  MX_DISP_PMODE,
  MX_DISP_DISPFB1,
  MX_DISP_DISPLAY1,
  MX_DISP_DISPFB2,
  MX_DISP_DISPLAY2,
  MX_DISP_BGCOLOR,
  MX_DISP_NREGS
};
void mx_display_regs(const struct mx_video_mode *m, const struct mx_display *d,
                     u64 out[MX_DISP_NREGS]);
/* The magnification circuit c gets in mode m (its own, or the fitted one). */
u32 mx_circuit_magh(const struct mx_video_mode *m, const struct mx_circuit *c);
u32 mx_circuit_magv(const struct mx_video_mode *m, const struct mx_circuit *c);

/* Write d's registers now (they are write-only: mx_gs.dregs keeps them). */
void mx_display_apply(const struct mx_display *d);
/* Write them at the next vblank. d must stay valid until then. */
void mx_display_show(const struct mx_display *d);

#endif
