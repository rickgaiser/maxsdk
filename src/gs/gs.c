#include <mx/bios.h>
#include <mx/dma.h>
#include <mx/emit.h>
#include <mx/gs.h>
#include <mx/irq.h>
#include <mx/sif.h>
#include <mx/tex.h>
#include <mx/trace.h>
#include <mx/vif.h>

struct mx_gs mx_gs;

#define CSR MX_R64(MX_GS_CSR)
#define IMR_ON 0x7C00ull /* SIGNAL and FINISH unmasked, the rest masked */

/* write a display's registers (with interrupts off or from a handler). The
   registers of a circuit that is off are not written: it keeps its last
   (valid) rectangle, as zeros there are not a valid picture. */
static void apply(const struct mx_display *d) {
  static const u32 reg[MX_DISP_NREGS] = {MX_GS_PMODE,   MX_GS_DISPFB1,  MX_GS_DISPLAY1,
                                         MX_GS_DISPFB2, MX_GS_DISPLAY2, MX_GS_BGCOLOR};
  u64 r[MX_DISP_NREGS];
  mx_display_regs(&mx_video, d, r);
  for (int i = 0; i < MX_DISP_NREGS; i++) {
    if ((i == MX_DISP_DISPFB1 || i == MX_DISP_DISPLAY1) && !d->c[0].on)
      continue;
    if ((i == MX_DISP_DISPFB2 || i == MX_DISP_DISPLAY2) && !d->c[1].on)
      continue;
    MX_R64(reg[i]) = r[i];
    mx_gs.dregs[i] = r[i];
  }
  mx_gs.shown_fbp = d->c[1].on ? d->c[1].fbp : d->c[0].fbp;
}

static void show(u32 frame) {
  if (mx_gs.ndisp)
    apply(&mx_gs.disp[frame % mx_gs.ndisp]);
}

void mx_display_apply(const struct mx_display *d) {
  int was = mx_di();
  apply(d);
  mx_irq_restore(was);
}

void mx_display_show(const struct mx_display *d) { mx_gs.pending = d; }

/* the displays of mx_gs_init */
static struct mx_display def[2];

void mx_gs_displays(const struct mx_display *d, u32 n) {
  int was = mx_di();
  mx_gs.disp = d ? d : def;
  mx_gs.ndisp = d ? n : 2;
  mx_irq_restore(was);
}

void mx_gs_smooth(int mode) {
  int on = mode != MX_SMOOTH_OFF;
  mx_gs.smooth = mode;
  for (int i = 0; i < 2; i++) {
    struct mx_display *d = &def[i];
    /* circuit 1 the same, half a pixel to the right or a line down, blended
       1:1 (ALP 0x80) over circuit 2; or circuit 2 only */
    d->c[0] = d->c[1];
    d->c[0].on = (u8)on;
    if (mode == MX_SMOOTH_ACROSS)
      d->c[0].dx += (s32)mx_circuit_magh(&mx_video, &d->c[1]) / 2;
    if (mode == MX_SMOOTH_DOWN)
      d->c[0].dby++;
    d->amod = (u8)!on;
    d->alp = on ? 0x80 : 0;
  }
  if (mx_gs.disp == def)
    mx_display_apply(&def[(mx_gs.shown ? mx_gs.shown - 1 : 0) & 1]);
}

u32 mx_video_field(void) { return (u32)MX_GET(GS_CSR_FIELD, CSR); }

void mx_vram_reset(void) { mx_gs.vram = mx_gs.tbp = 0; }

u32 mx_vram_alloc(u32 pages) {
  u32 p = mx_gs.vram;
  if (p + pages > 512)
    return ~0u;
  mx_gs.vram = p + pages;
  mx_gs.tbp = MX_GS_PAGE(mx_gs.vram);
  return p;
}

/* A present (mx_frame_present) starts this long after the vblank at the
   latest: it is drawn before the picture's first line (NTSC: about 1.6 ms
   after the vblank starts; a present of the whole screen takes 0.4 ms). */
#define PRESENT_LATE 235930u /* 0.8 ms */

/* VIF1 stops at the I-bit MARK of a frame's present until the frame is
   drawn (its FINISH: mx_gs.frames_done reaches the MARK); let it go on. */
static int present_release(void) {
  u32 code = MX_R32(MX_VIF_VIF1_CODE);
  /* bit 15: the PATH3 stream's marks (frame3.c) */
  if (!(MX_R32(MX_VIF_VIF1_STAT) & MX_BIT(VIF_STAT_VIS)) ||
      code >> 24 != (MX_VIF_IBIT >> 24 | MX_VIF_MARK) || (code & 0x8000) ||
      (mx_gs.frames_done - code) & 0x4000)
    return 0;
  MX_R32(MX_VIF_VIF1_FBRST) = (u32)MX_BIT(VIF_FBRST_STC);
  return 1;
}

static void gs_irq(void *arg) {
  u64 csr = CSR;
  if (csr & MX_BIT(GS_CSR_SIGNAL)) {
    mx_gs.last_sigid = (u32)MX_R64(MX_GS_SIGLBLID);
    mx_gs.signal++;
    mx_trace(MX_TR_GS_SIGNAL, (u16)mx_gs.last_sigid);
  }
  if (csr & MX_BIT(GS_CSR_FINISH)) {
    mx_gs.finish++;
    mx_gs.finish_at = mx_count();
    if (mx_gs.frames_done < mx_gs.frames)
      mx_gs.frames_done++;
    /* without vsync: show it now (it tears), so the next frame, which
       starts after this FINISH, draws into the buffer that is not shown */
    if (mx_gs.immediate && mx_gs.shown != mx_gs.frames_done) {
      mx_gs.shown = mx_gs.frames_done;
      show(mx_gs.shown - 1);
    }
    /* a present: now without vsync, or if the vblank has only just begun */
    if ((mx_gs.immediate || mx_count() - mx_gs.vsync_at < PRESENT_LATE) && present_release() &&
        !mx_gs.immediate && mx_gs.shown != mx_gs.frames_done) {
      mx_gs.shown = mx_gs.frames_done;
      mx_gs.slack = 0;
      show(mx_gs.shown - 1);
    }
    mx_trace(MX_TR_GS_FINISH, (u16)mx_gs.finish);
  }
  /* acknowledge what we saw (writing 1 clears) */
  CSR = csr & (MX_BIT(GS_CSR_SIGNAL) | MX_BIT(GS_CSR_FINISH));
}

static void vsync_irq(void *arg) {
  mx_gs.vsync++;
  mx_gs.vsync_at = mx_count();
  mx_trace(MX_TR_VSYNC, (u16)mx_gs.vsync);
  present_release();
  if (mx_gs.pending) {
    apply(mx_gs.pending);
    mx_gs.pending = 0;
  }
  /* show the newest finished frame */
  if (mx_gs.frames_done && mx_gs.shown != mx_gs.frames_done) {
    mx_gs.shown = mx_gs.frames_done;
    mx_gs.slack = mx_count() - mx_gs.finish_at;
    show(mx_gs.shown - 1);
  } else if (mx_gs.counting && !mx_gs.immediate && mx_gs.shown > mx_gs.first) {
    if (mx_gs.frames != mx_gs.frames_done)
      mx_gs.missed++;
    else
      mx_gs.starved++;
  }
}

void mx_gs_init(u32 w, u32 h) { mx_gs_init_mode(w, h, 0); }

/* GS reset and the CRTC timing; the GS interrupts masked */
static void crt(const struct mx_video_mode *m) {
  mx_video = *m;
  mx_gs.hz = m->hz;
  MX_R32(MX_GIF_CTRL) = (u32)MX_BIT(GIF_CTRL_RST);
  CSR = MX_BIT(GS_CSR_RESET);
  mx_bios_gs_put_imr(0xFF00);
  mx_bios_set_gs_crt(m->interlace, m->code, m->ffmd);
  if (m->interlace)
    MX_R64(MX_GS_SMODE2) = MX_SET(GS_SMODE2_INT, 1) | MX_SET(GS_SMODE2_FFMD, m->ffmd);
}

static void irq_on(void) {
  CSR = MX_BIT(GS_CSR_SIGNAL) | MX_BIT(GS_CSR_FINISH);
  mx_irq_intc(MX_INTC_GS, gs_irq, 0);
  mx_irq_intc(MX_INTC_VBON, vsync_irq, 0);
  mx_bios_gs_put_imr(IMR_ON);
}

/* Two vblanks within 100 ms: a mode the console does not have gives none
   (576p on BIOS 2.00, PS2 2026-10-27). */
static int vblanks(void) {
  u32 v = mx_gs.vsync, t0 = mx_count();
  while (mx_gs.vsync - v < 2)
    if (mx_count() - t0 > 29491200u)
      return -1;
  return 0;
}

int mx_video_set(const struct mx_video_mode *m) {
  crt(m);
  irq_on();
  return vblanks();
}

static void reset_state(void) {
  mx_gs.frames = mx_gs.frames_done = mx_gs.shown = 0;
  mx_gs.missed = mx_gs.starved = mx_gs.counting = mx_gs.immediate = mx_gs.first = 0;
  mx_gs.smooth = 0;
  mx_gs.disp = 0;
  mx_gs.ndisp = 0;
  mx_gs.pending = 0;
  mx_vram_reset();
  mx_dma_init();
}

int mx_gs_init_video(const struct mx_video_mode *m) {
  reset_state();
  return mx_video_set(m);
}

void mx_gs_init_mode(u32 w, u32 h, u32 flags) {
  /* interlaced NTSC or PAL. Up to 256 lines: FRAME mode, each field shows
     every line (both fields the same image, half a line apart); more: FIELD
     mode, each field shows every other line of the buffer */
  struct mx_video_mode m = flags & MX_GS_PAL ? mx_video_pal : mx_video_ntsc;
  m.ffmd = h <= 256;
  reset_state();
  crt(&m);

  mx_gs.w = w;
  mx_gs.h = h;
  mx_gs.fbw = (w + 63) / 64;
  u32 pages = mx_rt_pages(w, h, MX_GS_PSM_CT32);
  mx_gs.fbp[0] = mx_vram_alloc(pages);
  mx_gs.fbp[1] = mx_vram_alloc(pages);
  mx_gs.zbp = mx_vram_alloc(pages);
  for (int i = 0; i < 2; i++) {
    mx_rt_init(&mx_gs.rt[i], mx_gs.fbp[i], w, h, MX_GS_PSM_CT32, mx_gs.zbp, MX_GS_PSM_Z24);
    mx_display_init(&def[i], &mx_gs.rt[i]);
  }
  mx_gs_displays(def, 2);
  /* shown before the interrupts are on, as before */
  apply(&def[0]);
  irq_on();
}

void mx_gs_draw_env(struct mx_pkt *p, int buf) {
  u32 w = mx_gs.w, h = mx_gs.h;
  mx_pkt_ad_begin(p, 8, 1);
  mx_pkt_ad(p, MX_GS_REG_FRAME_1, mx_gs_frame(mx_gs.fbp[buf], mx_gs.fbw, MX_GS_PSM_CT32, 0));
  mx_pkt_ad(p, MX_GS_REG_ZBUF_1, mx_gs_zbuf(mx_gs.zbp, MX_GS_PSM_Z24, 0));
  mx_pkt_ad(p, MX_GS_REG_XYOFFSET_1, mx_gs_xyoffset((2048 - w / 2) << 4, (2048 - h / 2) << 4));
  mx_pkt_ad(p, MX_GS_REG_SCISSOR_1, mx_gs_scissor(0, w - 1, 0, h - 1));
  /* Z test on, always pass (overwritten by programs that want GEQUAL) */
  mx_pkt_ad(p, MX_GS_REG_TEST_1, mx_gs_test(0, 0, 0, 0, 0, 0, 1, 1));
  mx_pkt_ad(p, MX_GS_REG_PRMODECONT, 1);
  mx_pkt_ad(p, MX_GS_REG_COLCLAMP, 1);
  mx_pkt_ad(p, MX_GS_REG_DTHE, 0);
}

void mx_gs_clear(struct mx_pkt *p, u32 r, u32 g, u32 b) {
  mx_pkt_ad_begin(p, 4, 1);
  mx_pkt_ad(p, MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 0, 0, 0, 0, 0, 0, 0));
  mx_pkt_ad(p, MX_GS_REG_RGBAQ, mx_gs_rgbaq(r, g, b, 0x80, 1.0f));
  mx_pkt_ad(p, MX_GS_REG_XYZ2, mx_gs_xyz(mx_gs_px(0), mx_gs_py(0), 0));
  mx_pkt_ad(p, MX_GS_REG_XYZ2, mx_gs_xyz(mx_gs_px(mx_gs.w), mx_gs_py(mx_gs.h), 0));
}

void mx_gs_finish_ad(struct mx_pkt *p) {
  mx_pkt_ad_begin(p, 1, 1);
  mx_pkt_ad(p, MX_GS_REG_FINISH, 1);
}

void mx_gs_signal_ad(struct mx_pkt *p, u32 id) {
  mx_pkt_ad_begin(p, 1, 1);
  mx_pkt_ad(p, MX_GS_REG_SIGNAL, mx_gs_signal(id, 0xFFFFFFFFu));
}

void mx_gs_label_ad(struct mx_pkt *p, u32 id) {
  mx_pkt_ad_begin(p, 1, 1);
  mx_pkt_ad(p, MX_GS_REG_LABEL, mx_gs_label(id, 0xFFFFFFFFu));
}

void mx_gs_upload(struct mx_pkt *p, u32 dbp, u32 dbw, u32 psm, u32 x, u32 y, u32 w, u32 h,
                  const void *data) {
  u32 bpp = psm == MX_GS_PSM_CT32 || psm == MX_GS_PSM_Z32 ? 32
            : psm == MX_GS_PSM_CT24 || psm == MX_GS_PSM_Z24 ? 24
            : psm == MX_GS_PSM_T8 ? 8
            : psm == MX_GS_PSM_T4 ? 4
                                  : 16;
  u32 qwc = (w * h * bpp / 8 + 15) / 16;
  mx_pkt_ad_begin(p, 4, 0);
  mx_pkt_ad(p, MX_GS_REG_BITBLTBUF, mx_gs_bitbltbuf(0, 0, 0, dbp, dbw, psm));
  mx_pkt_ad(p, MX_GS_REG_TRXPOS, mx_gs_trxpos(0, 0, x, y, 0));
  mx_pkt_ad(p, MX_GS_REG_TRXREG, mx_gs_trxreg(w, h));
  mx_pkt_ad(p, MX_GS_REG_TRXDIR, 0);
  const u8 *d = data;
  while (qwc) {
    u32 n = qwc > 0x7FFF ? 0x7FFF : qwc;
    mx_pkt_giftag(p, n, n == qwc, -1, MX_GIF_FLG_IMAGE, 0, 0);
    mx_pkt_ref(p, d, n, 0, 0);
    mx_pkt_cnt(p, 0, 0);
    d += n * 16;
    qwc -= n;
  }
  /* the texture cache must not keep old data */
  mx_pkt_ad_begin(p, 1, 1);
  mx_pkt_ad(p, MX_GS_REG_TEXFLUSH, 0);
}

void mx_tex_upload(struct mx_pkt *p, const struct mx_tex *t, u32 level, const void *data) {
  mx_gs_upload(p, t->tbp[level], t->tbw[level], t->psm, 0, 0, 1u << (t->tw - level),
               1u << (t->th - level), data);
}

u64 *mx_gs_e_draw_env(u64 *c, int buf, int eop) {
  c = mx_rt_e_regs(mx_e_ad_tag(c, 8, eop), &mx_gs.rt[buf], 0);
  c = mx_e_ad(c, MX_GS_REG_TEST_1, mx_gs_test(0, 0, 0, 0, 0, 0, 1, 1));
  c = mx_e_ad(c, MX_GS_REG_PRMODECONT, 1);
  c = mx_e_ad(c, MX_GS_REG_COLCLAMP, 1);
  return mx_e_ad(c, MX_GS_REG_DTHE, 0);
}

u64 *mx_gs_e_clear(u64 *c, u32 r, u32 g, u32 b, int eop) {
  return mx_rt_e_clear(c, &mx_gs.rt[0], 0, r, g, b, eop);
}

void mx_gs_send(const void *chain) {
  mx_dma_wait(MX_CH_GIF);
  mx_dma_chain(MX_CH_GIF, chain, 0);
  mx_dma_wait(MX_CH_GIF);
}

int mx_gs_wait_finish(u32 since, u32 n) {
  u32 t0 = mx_count();
  while (mx_gs.finish - since < n)
    if (mx_count() - t0 > 300000000u)
      return -1;
  return 0;
}

void mx_gs_wait_vsync(void) {
  u32 v = mx_gs.vsync;
  while (mx_gs.vsync == v)
    ;
}

int mx_gs_frame_begin(void) {
  u32 n = mx_gs.frames; /* the frame about to be built */
  /* frame n uses the buffer of frame n - 2, which must have been replaced
     on screen by frame n - 1 */
  if (n >= 2)
    while (mx_gs.shown < n)
      ;
  return (int)(n & 1);
}

void mx_gs_frame_submit(const void *chain) {
  mx_dma_wait(MX_CH_GIF);
  mx_gs.frames++;
  mx_dma_chain(MX_CH_GIF, chain, 0);
}

int mx_gs_download(void *dst, u32 bp, u32 bw, u32 psm, u32 x, u32 y, u32 w, u32 h) {
  static u32 setup[16 * 4] MX_ALIGNED(64);
  u32 bytes = w * h * 4;
  if (bytes % 128)
    return -1;
  struct mx_pkt p;
  mx_pkt_begin(&p, MX_UNCACHED(setup), sizeof setup, 0);
  mx_pkt_end(&p, 0, 0);
  mx_pkt_ad_begin(&p, 5, 1);
  mx_pkt_ad(&p, MX_GS_REG_BITBLTBUF, mx_gs_bitbltbuf(bp, bw, psm, 0, 0, 0));
  mx_pkt_ad(&p, MX_GS_REG_TRXPOS, mx_gs_trxpos(x, y, 0, 0, 0));
  mx_pkt_ad(&p, MX_GS_REG_TRXREG, mx_gs_trxreg(w, h));
  mx_pkt_ad(&p, MX_GS_REG_FINISH, 0);
  mx_pkt_ad(&p, MX_GS_REG_TRXDIR, 1);
  mx_pkt_finish(&p);
  u32 f0 = mx_gs.finish;
  mx_gs_send(setup);
  if (mx_gs_wait_finish(f0, 1) < 0)
    return -2;
  /* reverse the VIF1 FIFO and the GS bus, read with VIF1 DMA */
  MX_R32(MX_VIF_VIF1_STAT) = (u32)MX_BIT(VIF_STAT_FDR);
  MX_R64(MX_GS_BUSDIR) = 1;
  mx_dcache_invalidate(dst, bytes);
  MX_DMA_REG(MX_CH_VIF1, MADR) = mx_phys(dst);
  MX_DMA_REG(MX_CH_VIF1, QWC) = bytes / 16;
  mx_sync();
  MX_DMA_REG(MX_CH_VIF1, CHCR) = (u32)(MX_CHCR_NORMAL | MX_CHCR_TO_MEM | MX_CHCR_STR);
  int r = mx_dma_wait(MX_CH_VIF1);
  MX_R64(MX_GS_BUSDIR) = 0;
  MX_R32(MX_VIF_VIF1_STAT) = 0;
  mx_dcache_invalidate(dst, bytes);
  return r;
}

int mx_gs_save_tga(int buf, const char *path) { return mx_gs_save_tga_rt(&mx_gs.rt[buf], path); }

int mx_gs_save_tga_rt(const struct mx_rt *t, const char *path) {
  static u32 line[1024] MX_ALIGNED(64);
  static u8 out[1024 * 4] MX_ALIGNED(64);
  u32 w = t->w, h = t->h, rep = mx_video_line_lines(&mx_video);
  if (t->psm != MX_GS_PSM_CT32 || w > 1024)
    return -1;
  int fd = mx_fio_open(path, MX_FIO_WRONLY | MX_FIO_CREAT | MX_FIO_TRUNC);
  if (fd < 0)
    return -1;
  /* uncompressed true colour, origin at the top left */
  u8 hdr[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, (u8)w, (u8)(w >> 8), (u8)(rep * h),
                (u8)(rep * h >> 8), 32, 0x28};
  int r = mx_fio_write(fd, hdr, sizeof hdr);
  for (u32 y = 0; y < h && r == 0; y++) {
    if (mx_gs_download(line, MX_GS_PAGE(t->fbp), t->fbw, MX_GS_PSM_CT32, 0, y, w, 1) < 0) {
      r = -1;
      break;
    }
    for (u32 x = 0; x < w; x++) {
      u32 p = line[x];
      out[4 * x] = (u8)(p >> 16), out[4 * x + 1] = (u8)(p >> 8), out[4 * x + 2] = (u8)p;
      out[4 * x + 3] = 0xFF;
    }
    for (u32 k = 0; k < rep && r == 0; k++)
      r = mx_fio_write(fd, out, 4 * w);
  }
  mx_fio_close(fd);
  return r;
}

int mx_gs_save_tga_fields(const struct mx_rt *even, const struct mx_rt *odd, const char *path) {
  static u32 line[1024] MX_ALIGNED(64);
  static u8 out[1024 * 4] MX_ALIGNED(64);
  u32 w = even->w, h = even->h;
  if (even->psm != MX_GS_PSM_CT32 || odd->psm != MX_GS_PSM_CT32 || w > 1024 || odd->w != w ||
      odd->h != h)
    return -1;
  int fd = mx_fio_open(path, MX_FIO_WRONLY | MX_FIO_CREAT | MX_FIO_TRUNC);
  if (fd < 0)
    return -1;
  /* uncompressed true colour, origin at the top left */
  u8 hdr[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, (u8)w, (u8)(w >> 8), (u8)(2 * h),
                (u8)(2 * h >> 8), 32, 0x28};
  int r = mx_fio_write(fd, hdr, sizeof hdr);
  for (u32 y = 0; y < 2 * h && r == 0; y++) {
    const struct mx_rt *t = y & 1 ? odd : even;
    if (mx_gs_download(line, MX_GS_PAGE(t->fbp), t->fbw, MX_GS_PSM_CT32, 0, y / 2, w, 1) < 0) {
      r = -1;
      break;
    }
    for (u32 x = 0; x < w; x++) {
      u32 p = line[x];
      out[4 * x] = (u8)(p >> 16), out[4 * x + 1] = (u8)(p >> 8), out[4 * x + 2] = (u8)p;
      out[4 * x + 3] = 0xFF;
    }
    r = mx_fio_write(fd, out, 4 * w);
  }
  mx_fio_close(fd);
  return r;
}
