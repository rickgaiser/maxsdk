#ifndef MX_GS_H
#define MX_GS_H

/* GS: display setup, GS interrupts (FINISH, SIGNAL, vsync), a double
   buffered frame loop, PATH3 upload and VRAM readback. No scene or texture
   management. */

#include <mx/pkt.h>
#include <mx/types.h>

struct mx_gs {
  u32 w, h;      /* framebuffer size in pixels */
  u32 fbw;       /* width in 64-pixel units */
  u32 fbp[2];    /* framebuffers (8 KiB pages) */
  u32 zbp;       /* Z buffer page */
  u32 tbp;       /* first free 256-byte block (textures) */
  u32 frames;    /* frames submitted */
  volatile u32 finish;    /* FINISH interrupts */
  volatile u32 signal;    /* SIGNAL interrupts */
  volatile u32 vsync;     /* vblank starts */
  volatile u32 shown;     /* number of the frame being displayed + 1 */
  volatile u32 frames_done; /* frames whose FINISH arrived */
  volatile u32 last_sigid;
  volatile u32 shown_fbp; /* DISPFB2.FBP last written (the register is write-only) */
};

extern struct mx_gs mx_gs;

/* FRAME/ZBUF/DISPFB count 2048-word pages, TEX0/BITBLTBUF 64-word blocks. */
#define MX_GS_PAGE(p) ((p) * 32)

/* Window pixel coordinates to primitive coordinates (12.4 fixed point,
   offset by the XYOFFSET that mx_gs_draw_env sets). */
MX_INLINE u32 mx_gs_px(u32 x) { return ((2048 - mx_gs.w / 2) + x) << 4; }
MX_INLINE u32 mx_gs_py(u32 y) { return ((2048 - mx_gs.h / 2) + y) << 4; }

/* NTSC, interlaced frame mode, w x h (h = lines per field, e.g. 640 x 224),
   32-bit color, 24-bit Z, two framebuffers. Installs the GS and vblank
   interrupt handlers. */
void mx_gs_init(u32 w, u32 h);

/* Append A+D writes that set up drawing into framebuffer buf (0/1): FRAME,
   ZBUF, XYOFFSET, SCISSOR, TEST, PRMODECONT, COLCLAMP, DTHE. 9 quadwords. */
void mx_gs_draw_env(struct mx_pkt *p, int buf);
/* Clear the whole framebuffer and Z with a sprite. 6 quadwords. */
void mx_gs_clear(struct mx_pkt *p, u32 r, u32 g, u32 b);
/* A+D write of FINISH / SIGNAL / LABEL (2 quadwords each). */
void mx_gs_finish_ad(struct mx_pkt *p);
void mx_gs_signal_ad(struct mx_pkt *p, u32 id);
void mx_gs_label_ad(struct mx_pkt *p, u32 id);
/* Host -> local transfer of w x h pixels to (x, y) of buffer dbp: setup
   registers and IMAGE tags in the packet, the data by ref tags. */
void mx_gs_upload(struct mx_pkt *p, u32 dbp, u32 dbw, u32 psm, u32 x, u32 y, u32 w, u32 h,
                  const void *data);

/* Send a GIF chain over PATH3 (GIF channel) and wait for the DMA. */
void mx_gs_send(const void *chain);
/* Wait until n more FINISH interrupts have arrived than at 'since'. */
int mx_gs_wait_finish(u32 since, u32 n);
void mx_gs_wait_vsync(void);

/* Frame loop: mx_gs_frame_begin() waits until the next back buffer is free
   (its previous frame is no longer shown) and returns it (0/1). The frame's
   GIF chain must end with mx_gs_finish_ad(); mx_gs_frame_submit() sends it.
   The vblank handler shows the newest finished frame. */
int mx_gs_frame_begin(void);
void mx_gs_frame_submit(const void *chain);

/* Local -> host readback of w x h pixels of a 32-bit buffer at (x, y) into
   dst. bp is in 64-word blocks (BITBLTBUF units; a framebuffer page is 32
   blocks: use MX_GS_PAGE(fbp)), bw in 64-pixel units. dst (uncached or cached; it is invalidated). w * h * 4 must be
   a multiple of 128 bytes. Blocking. */
int mx_gs_download(void *dst, u32 bp, u32 bw, u32 psm, u32 x, u32 y, u32 w, u32 h);

#endif
