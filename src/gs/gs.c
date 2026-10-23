#include <mx/bios.h>
#include <mx/dma.h>
#include <mx/emit.h>
#include <mx/gs.h>
#include <mx/irq.h>
#include <mx/trace.h>

struct mx_gs mx_gs;

#define CSR MX_R64(MX_GS_CSR)
#define IMR_ON 0x7C00ull /* SIGNAL and FINISH unmasked, the rest masked */

static void show(u32 frame) {
  u32 buf = frame & 1;
  mx_gs.shown_fbp = mx_gs.fbp[buf];
  MX_R64(MX_GS_DISPFB2) = MX_SET(GS_DISPFB2_FBP, mx_gs.fbp[buf]) |
                          MX_SET(GS_DISPFB2_FBW, mx_gs.fbw) |
                          MX_SET(GS_DISPFB2_PSM, MX_GS_PSM_CT32);
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
    if (mx_gs.frames_done < mx_gs.frames)
      mx_gs.frames_done++;
    mx_trace(MX_TR_GS_FINISH, (u16)mx_gs.finish);
  }
  /* acknowledge what we saw (writing 1 clears) */
  CSR = csr & (MX_BIT(GS_CSR_SIGNAL) | MX_BIT(GS_CSR_FINISH));
}

static void vsync_irq(void *arg) {
  mx_gs.vsync++;
  mx_trace(MX_TR_VSYNC, (u16)mx_gs.vsync);
  /* show the newest finished frame */
  if (mx_gs.frames_done && mx_gs.shown != mx_gs.frames_done) {
    mx_gs.shown = mx_gs.frames_done;
    show(mx_gs.shown - 1);
  } else if (mx_gs.counting) {
    if (mx_gs.frames != mx_gs.frames_done)
      mx_gs.missed++;
    else
      mx_gs.starved++;
  }
}

void mx_gs_init(u32 w, u32 h) {
  mx_gs.w = w;
  mx_gs.h = h;
  mx_gs.fbw = (w + 63) / 64;
  u32 pages = mx_gs.fbw * ((h + 31) / 32); /* 8 KiB pages per 32-bit buffer */
  mx_gs.fbp[0] = 0;
  mx_gs.fbp[1] = pages;
  mx_gs.zbp = 2 * pages;
  mx_gs.tbp = 3 * pages * 32;
  mx_gs.frames = mx_gs.frames_done = mx_gs.shown = 0;
  mx_gs.missed = mx_gs.starved = mx_gs.counting = 0;

  mx_dma_init();
  MX_R32(MX_GIF_CTRL) = (u32)MX_BIT(GIF_CTRL_RST);
  CSR = MX_BIT(GS_CSR_RESET);
  mx_bios_gs_put_imr(0xFF00);
  mx_bios_set_gs_crt(1, 2, 1); /* interlaced, NTSC, frame mode */

  /* read circuit 2 only (sceGsSetDefDispEnv's NTSC values) */
  MX_R64(MX_GS_PMODE) = 0x66;
  MX_R64(MX_GS_SMODE2) = 3;
  show(0);
  MX_R64(MX_GS_DISPLAY2) = MX_SET(GS_DISPLAY2_DX, 0x27C) | MX_SET(GS_DISPLAY2_DY, 50) |
                           MX_SET(GS_DISPLAY2_MAGH, (2560 + w - 1) / w - 1) |
                           MX_SET(GS_DISPLAY2_DW, 0x9FF) | MX_SET(GS_DISPLAY2_DH, (h << 1) - 1);
  MX_R64(MX_GS_BGCOLOR) = 0;

  CSR = MX_BIT(GS_CSR_SIGNAL) | MX_BIT(GS_CSR_FINISH);
  mx_irq_intc(MX_INTC_GS, gs_irq, 0);
  mx_irq_intc(MX_INTC_VBON, vsync_irq, 0);
  mx_bios_gs_put_imr(IMR_ON);
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

u64 *mx_gs_e_draw_env(u64 *c, int buf, int eop) {
  u32 w = mx_gs.w, h = mx_gs.h;
  c = mx_e_ad_tag(c, 8, eop);
  c = mx_e_ad(c, MX_GS_REG_FRAME_1, mx_gs_frame(mx_gs.fbp[buf], mx_gs.fbw, MX_GS_PSM_CT32, 0));
  c = mx_e_ad(c, MX_GS_REG_ZBUF_1, mx_gs_zbuf(mx_gs.zbp, MX_GS_PSM_Z24, 0));
  c = mx_e_ad(c, MX_GS_REG_XYOFFSET_1, mx_gs_xyoffset((2048 - w / 2) << 4, (2048 - h / 2) << 4));
  c = mx_e_ad(c, MX_GS_REG_SCISSOR_1, mx_gs_scissor(0, w - 1, 0, h - 1));
  c = mx_e_ad(c, MX_GS_REG_TEST_1, mx_gs_test(0, 0, 0, 0, 0, 0, 1, 1));
  c = mx_e_ad(c, MX_GS_REG_PRMODECONT, 1);
  c = mx_e_ad(c, MX_GS_REG_COLCLAMP, 1);
  return mx_e_ad(c, MX_GS_REG_DTHE, 0);
}

u64 *mx_gs_e_clear(u64 *c, u32 r, u32 g, u32 b, int eop) {
  c = mx_e_ad_tag(c, 4, eop);
  c = mx_e_ad(c, MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 0, 0, 0, 0, 0, 0, 0));
  c = mx_e_ad(c, MX_GS_REG_RGBAQ, mx_gs_rgbaq(r, g, b, 0x80, 1.0f));
  c = mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(mx_gs_px(0), mx_gs_py(0), 0));
  return mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(mx_gs_px(mx_gs.w), mx_gs_py(mx_gs.h), 0));
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
