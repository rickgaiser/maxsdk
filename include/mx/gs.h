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
  /* vblanks without a new frame to show: */
  volatile u32 missed;    /* a frame was submitted but its FINISH had not come (GS late) */
  volatile u32 starved;   /* no frame was submitted (EE late) */
  volatile int counting;  /* count missed/starved vblanks (between the first and last frame) */
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

/* The same as mx_gs_draw_env / mx_gs_clear with cursor emitters
   (<mx/emit.h>): 9 and 5 quadwords, GIF tags included. eop: the last
   GIF packet. */
u64 *mx_gs_e_draw_env(u64 *c, int buf, int eop);
u64 *mx_gs_e_clear(u64 *c, u32 r, u32 g, u32 b, int eop);

/* A cache of GS register values written by A+D, so a register is only
   written when its value changes. PRIM is not cached (GIF tags set it). */
struct mx_gs_regs {
  u64 v[0x64];
  u8 valid[0x64];
};
MX_INLINE void mx_gs_regs_reset(struct mx_gs_regs *r) {
  for (int i = 0; i < 0x64; i++)
    r->valid[i] = 0;
}
/* Append an A+D write of reg = v at c unless the cache has it. */
MX_INLINE u64 *mx_gs_e_reg(u64 *c, struct mx_gs_regs *r, u32 reg, u64 v) {
  if (r->valid[reg] && r->v[reg] == v)
    return c;
  r->valid[reg] = 1;
  r->v[reg] = v;
  c[0] = v; /* plain stores: c is read back by mx_gs_e_regs_close */
  c[1] = reg;
  return c + 2;
}
/* Start / close a run of mx_gs_e_reg: the tag gets the number of writes
   that were not skipped; with none, the tag is dropped (returns tag). */
MX_INLINE u64 *mx_gs_e_regs_begin(u64 *tag) { return tag + 2; }
MX_INLINE u64 *mx_gs_e_regs_close(u64 *tag, u64 *c, int eop) {
  u32 n = (u32)(c - tag) / 2 - 1;
  if (!n)
    return tag;
  tag[0] = mx_giftag(n, eop, -1, MX_GIF_FLG_PACKED, 1);
  tag[1] = MX_GIF_REG_AD;
  return c;
}

/* Local -> host readback of w x h pixels of a 32-bit buffer at (x, y) into
   dst. bp is in 64-word blocks (BITBLTBUF units; a framebuffer page is 32
   blocks: use MX_GS_PAGE(fbp)), bw in 64-pixel units. dst (uncached or cached; it is invalidated). w * h * 4 must be
   a multiple of 128 bytes. Blocking. */
int mx_gs_download(void *dst, u32 bp, u32 bw, u32 psm, u32 x, u32 y, u32 w, u32 h);

#endif
