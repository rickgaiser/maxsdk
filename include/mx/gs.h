#ifndef MX_GS_H
#define MX_GS_H

/* GS: setup and interrupts (FINISH, SIGNAL, vsync), the display of each
   frame, a double buffered frame loop, PATH3 upload and VRAM readback. No
   scene or texture management.

   Two ways to set it up:
   - mx_gs_init(w, h): NTSC/PAL interlaced, two 32-bit framebuffers and a
     24-bit Z buffer of w x h (mx_gs.rt), shown full screen on read circuit 2.
   - mx_gs_init_video(mode): only the video mode (<mx/video.h>). The
     application places its render targets (<mx/rt.h>, mx_vram_alloc) and
     says what each frame shows (<mx/display.h>, mx_gs_displays). */

#include <mx/display.h>
#include <mx/pkt.h>
#include <mx/rt.h>
#include <mx/types.h>
#include <mx/video.h>

struct mx_gs {
  u32 w, h;      /* framebuffer size in pixels */
  u32 fbw;       /* width in 64-pixel units */
  u32 fbp[2];    /* framebuffers (8 KiB pages) */
  u32 zbp;       /* Z buffer page */
  u32 hz;        /* vblanks a second: 60 (NTSC), 50 (PAL) */
  u32 tbp;       /* first free 256-byte block (textures) */
  u32 frames;    /* frames submitted */
  volatile u32 finish;    /* FINISH interrupts */
  volatile u32 signal;    /* SIGNAL interrupts */
  volatile u32 vsync;     /* vblank starts */
  volatile u32 vsync_at;  /* mx_count() at the last one */
  volatile u32 shown;     /* number of the frame being displayed + 1 */
  volatile u32 frames_done; /* frames whose FINISH arrived */
  volatile u32 last_sigid;
  volatile u32 shown_fbp; /* DISPFB2.FBP last written (the register is write-only) */
  /* vblanks without a new frame to show: */
  volatile u32 missed;    /* a frame was submitted but its FINISH had not come (GS late) */
  volatile u32 starved;   /* no frame was submitted (EE late) */
  volatile int counting;  /* count missed/starved vblanks (between the first and last frame) */
  u32 first;              /* the first frame of the frame pipeline's run: the vblanks are
                             counted once it is shown (its vblank comes at any time) */
  volatile int immediate; /* show each frame at its FINISH, not at the next vblank */
  volatile u32 finish_at; /* mx_count() at the last FINISH */
  volatile u32 slack;     /* EE cycles from the FINISH of the frame shown last to its vblank:
                             how early it was ready (vsync on) */
  volatile int smooth;    /* the display filter (mx_gs_smooth) */
  struct mx_rt rt[2];     /* mx_gs_init's framebuffers (and Z buffer) */
  const struct mx_display *disp; /* frame k is shown with disp[k % ndisp] */
  u32 ndisp;
  const struct mx_display *volatile pending; /* mx_display_show: at the next vblank */
  u64 dregs[MX_DISP_NREGS]; /* the display registers last written (write-only registers) */
  u32 vram;                 /* the first page that mx_vram_alloc has not handed out */
};

extern struct mx_gs mx_gs;

/* FRAME/ZBUF/DISPFB count 2048-word pages, TEX0/BITBLTBUF 64-word blocks. */
#define MX_GS_PAGE(p) ((p) * 32)

/* Window pixel coordinates to primitive coordinates (12.4 fixed point,
   offset by the XYOFFSET that mx_gs_draw_env sets). */
MX_INLINE u32 mx_gs_px(u32 x) { return ((2048 - mx_gs.w / 2) + x) << 4; }
MX_INLINE u32 mx_gs_py(u32 y) { return ((2048 - mx_gs.h / 2) + y) << 4; }

/* NTSC, interlaced, w x h, 32-bit color, 24-bit Z, two framebuffers. Up to
   256 lines FRAME mode (each field shows every line, e.g. 640 x 224), more
   FIELD mode (each field every other line, 640 x 448). Installs the GS and
   vblank interrupt handlers. */
void mx_gs_init(u32 w, u32 h);
/* The same, with flags: MX_GS_PAL, 50 Hz and up to 512 lines (NTSC: 60 Hz,
   up to 448; 640 x 512 leaves 256 KiB of GS memory). */
#define MX_GS_PAL 1u
void mx_gs_init_mode(u32 w, u32 h, u32 flags);
/* DMAC, the GS and vblank interrupts and video mode m; nothing is shown
   and no GS memory is handed out yet (mx_vram_alloc). Returns mx_video_set's
   result: -1 if the console does not have the mode. */
int mx_gs_init_video(const struct mx_video_mode *m);

/* GS memory for render targets, in 8 KiB pages from page 0 up: returns the
   first page, or ~0u if fewer than pages are left (512 in all). mx_gs.tbp is
   the 64-word block after the last page handed out, where textures can
   start. mx_gs_init* reset it. */
u32 mx_vram_alloc(u32 pages);
void mx_vram_reset(void);

/* The frame pipeline (mx_gs_frame_*, <mx/frame.h>) shows frame k with
   d[k % n]: frame k draws into the buffers d[k % n] shows, and can change
   d[k % n] (e.g. a fade's alpha) until it ends. n >= 2 buffers deep: frame k
   waits until frame k - n + 1 is shown. The array must stay valid.
   mx_gs_init* set two displays of mx_gs.rt; d = 0: those again. */
void mx_gs_displays(const struct mx_display *d, u32 n);

/* The display filter, on or off: both read circuits show the frame, the
   first half a pixel to the right, blended 1:1 by the PCRTC. It smooths the
   edges across at no drawing cost, every line its own (the picture is a
   little softer across). Only the display: not GS memory (screenshots).
   Changes the displays of mx_gs_init (not those of mx_gs_displays). */
void mx_gs_smooth(int on);

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

/* Save framebuffer buf (0/1) as a TGA file through the IOP file server
   (mx_fio_open), e.g. "host:shot.tga"; each field line is written twice, so
   the picture has the shape of the display. Blocking, slow (seconds).
   Returns 0, or -1. */
int mx_gs_save_tga(int buf, const char *path);
/* The same for target t (32-bit, at most 1024 wide); its lines are written
   twice when the mode shows each line in both fields. */
int mx_gs_save_tga_rt(const struct mx_rt *t, const char *path);

#endif
