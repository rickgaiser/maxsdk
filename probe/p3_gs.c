/* Phase 3 measurements (PS2 only): FINISH/SIGNAL interrupt latency, PATH1/2/3
   throughput, PATH3 next to PATH2 with and without GIF_MODE.IMT, fill rate,
   vsync period, and the cost of building packets. */

#include <mx/mx.h>

#define REPS 8
extern const u8 kick_vu[], kick_vu_end[];

static u32 img[64 * 1024 / 4] MX_ALIGNED(64);
static u32 pk[8192] MX_ALIGNED(64);
static u32 pk2[256] MX_ALIGNED(64); /* PATH2 packet, separate so it can run next to PATH3 */

static u32 median(u32 *v, int n) {
  for (int i = 1; i < n; i++)
    for (int j = i; j > 0 && v[j] < v[j - 1]; j--) {
      u32 t = v[j];
      v[j] = v[j - 1];
      v[j - 1] = t;
    }
  return v[n / 2];
}

static void report(const char *name, u32 *v, int n, const char *unit) {
  u32 mn = 0xFFFFFFFFu;
  for (int i = 0; i < n; i++)
    if (v[i] < mn)
      mn = v[i];
  mx_printf("%-44s min %7u med %7u %s\n", name, mn, median(v, n), unit);
}

/* Time from starting the GIF DMA of a small packet ending in FINISH or
   SIGNAL to the GS interrupt handler (from the trace). */
static void irq_latency(void) {
  u32 fin[REPS], sig[REPS];
  for (int k = 0; k < 2; k++)
    for (int r = 0; r < REPS; r++) {
      struct mx_pkt p;
      mx_pkt_begin(&p, MX_UNCACHED(pk), sizeof pk, 0);
      mx_pkt_end(&p, 0, 0);
      if (k)
        mx_gs_signal_ad(&p, (u32)r);
      else
        mx_gs_finish_ad(&p);
      mx_pkt_finish(&p);
      mx_trace_clear();
      mx_trace_enable(1);
      u32 f0 = mx_gs.finish, s0 = mx_gs.signal;
      u32 t0 = mx_count();
      mx_dma_chain(MX_CH_GIF, pk, 0);
      while (mx_gs.finish == f0 && mx_gs.signal == s0)
        ;
      mx_trace_enable(0);
      struct mx_trace_ev ev[16];
      u32 n = mx_trace_get(ev, 16), t = 0;
      for (u32 i = 0; i < n; i++)
        if (ev[i].ev == MX_TR_INTC_IRQ && ev[i].arg == MX_INTC_GS)
          t = ev[i].t;
      (k ? sig : fin)[r] = t - t0;
      mx_dma_wait(MX_CH_GIF);
    }
  report("gs: FINISH, DMA start to handler", fin, REPS, "cycles");
  report("gs: SIGNAL, DMA start to handler", sig, REPS, "cycles");
}

/* Host -> local IMAGE transfer of 64 KiB over PATH3 (GIF channel) or PATH2
   (VIF1 DIRECT). */
static u32 image_path3(void) {
  struct mx_pkt p;
  mx_pkt_begin(&p, pk, sizeof pk, 0);
  mx_pkt_cnt(&p, 0, 0);
  mx_gs_upload(&p, mx_gs.tbp, 4, MX_GS_PSM_CT32, 0, 0, 256, 64, img);
  mx_gs_finish_ad(&p);
  mx_pkt_end(&p, 0, 0);
  mx_pkt_finish(&p);
  mx_pkt_writeback(&p);
  u32 f0 = mx_gs.finish, t0 = mx_count();
  mx_dma_chain(MX_CH_GIF, pk, 0);
  mx_gs_wait_finish(f0, 1);
  return mx_count() - t0;
}

static u32 image_path2(void) {
  struct mx_pkt p;
  mx_pkt_begin(&p, pk2, sizeof pk2, MX_PKT_VIF);
  mx_pkt_cnt(&p, 0, 0);
  mx_pkt_direct(&p, 5 + 1);
  mx_pkt_ad_begin(&p, 4, 0);
  mx_pkt_ad(&p, MX_GS_REG_BITBLTBUF, mx_gs_bitbltbuf(0, 0, 0, mx_gs.tbp, 4, MX_GS_PSM_CT32));
  mx_pkt_ad(&p, MX_GS_REG_TRXPOS, mx_gs_trxpos(0, 0, 0, 0, 0));
  mx_pkt_ad(&p, MX_GS_REG_TRXREG, mx_gs_trxreg(256, 64));
  mx_pkt_ad(&p, MX_GS_REG_TRXDIR, 0);
  mx_pkt_giftag(&p, 4096, 1, -1, MX_GIF_FLG_IMAGE, 0, 0);
  /* the image itself, DIRECT in a ref packet */
  mx_pkt_ref(&p, img, 4096, 0, mx_vif_direct(4096));
  mx_pkt_end(&p, 0, 0);
  mx_pkt_finish(&p);
  mx_pkt_writeback(&p);
  u32 t0 = mx_count();
  mx_dma_chain(MX_CH_VIF1, pk2, 1);
  mx_dma_wait(MX_CH_VIF1);
  return mx_count() - t0;
}

/* PATH1: VU1 XGKICKs an IMAGE packet of 960 qw from its memory. */
static u32 image_path1(void) {
  static u32 vp[64] MX_ALIGNED(64);
  struct mx_pkt p;
  /* GIF tags at VU1 address 0: A+D setup then IMAGE of 960 qw; the data is
     whatever is in VU1 memory */
  u64 tagq[14] MX_ALIGNED(16);
  tagq[0] = mx_giftag(4, 0, -1, MX_GIF_FLG_PACKED, 1);
  tagq[1] = MX_GIF_REG_AD;
  tagq[2] = mx_gs_bitbltbuf(0, 0, 0, mx_gs.tbp, 4, MX_GS_PSM_CT32), tagq[3] = MX_GS_REG_BITBLTBUF;
  tagq[4] = mx_gs_trxpos(0, 0, 0, 0, 0), tagq[5] = MX_GS_REG_TRXPOS;
  tagq[6] = mx_gs_trxreg(256, 15), tagq[7] = MX_GS_REG_TRXREG;
  tagq[8] = 0, tagq[9] = MX_GS_REG_TRXDIR;
  tagq[10] = mx_giftag(960, 1, -1, MX_GIF_FLG_IMAGE, 0);
  tagq[11] = 0;
  mx_pkt_begin(&p, vp, sizeof vp, MX_PKT_VIF);
  mx_pkt_end(&p, mx_vif_stcycl(1, 1), 0);
  mx_pkt_unpack(&p, MX_UNPACK_V4_32, 0, 6, 0, tagq);
  mx_pkt_vif(&p, mx_vif_mscal(0));
  mx_pkt_vif(&p, mx_vif_flush());
  mx_pkt_finish(&p);
  mx_pkt_writeback(&p);
  u32 t0 = mx_count();
  mx_dma_chain(MX_CH_VIF1, vp, 1);
  mx_dma_wait(MX_CH_VIF1);
  return mx_count() - t0;
}

static void throughput(void) {
  u32 v[REPS];
  for (int r = 0; r < REPS; r++)
    v[r] = image_path3() * 100 / 4096;
  report("path3: 64 KiB IMAGE (GIF channel)", v, REPS, "cycles/100 qw");
  for (int r = 0; r < REPS; r++)
    v[r] = image_path2() * 100 / 4096;
  report("path2: 64 KiB IMAGE (VIF1 DIRECT)", v, REPS, "cycles/100 qw");
  for (int r = 0; r < REPS; r++)
    v[r] = image_path1() * 100 / 960;
  report("path1: 960 qw IMAGE (VU1 XGKICK, incl. start)", v, REPS, "cycles/100 qw");
}

/* PATH3 upload while PATH2 streams too, with GIF_MODE.IMT 0 and 1. */
static void interleave(void) {
  for (int imt = 0; imt < 2; imt++) {
    MX_R32(MX_GIF_MODE) = imt ? (u32)MX_BIT(GIF_MODE_IMT) : 0;
    u32 v[REPS];
    for (int r = 0; r < REPS; r++) {
      struct mx_pkt p;
      mx_pkt_begin(&p, pk, sizeof pk, 0);
      mx_pkt_cnt(&p, 0, 0);
      mx_gs_upload(&p, mx_gs.tbp + 512, 4, MX_GS_PSM_CT32, 0, 0, 256, 64, img);
      mx_gs_finish_ad(&p);
      mx_pkt_end(&p, 0, 0);
      mx_pkt_finish(&p);
      mx_pkt_writeback(&p);
      u32 f0 = mx_gs.finish, t0 = mx_count();
      mx_dma_chain(MX_CH_GIF, pk, 0);
      image_path2();
      mx_gs_wait_finish(f0, 1);
      v[r] = mx_count() - t0;
    }
    report(imt ? "path3+path2 64 KiB each, IMT=1" : "path3+path2 64 KiB each, IMT=0", v, REPS,
           "cycles");
  }
  MX_R32(MX_GIF_MODE) = 0;
}

static void fill(void) {
  u32 v[REPS];
  for (int r = 0; r < REPS; r++) {
    struct mx_pkt p;
    mx_pkt_begin(&p, pk, sizeof pk, 0);
    mx_pkt_cnt(&p, 0, 0);
    mx_gs_draw_env(&p, 0);
    mx_gs_finish_ad(&p);
    mx_pkt_end(&p, 0, 0);
    mx_pkt_finish(&p);
    mx_pkt_writeback(&p);
    u32 f0 = mx_gs.finish;
    mx_gs_send(pk);
    mx_gs_wait_finish(f0, 1);
    mx_pkt_begin(&p, pk, sizeof pk, 0);
    mx_pkt_cnt(&p, 0, 0);
    mx_gs_clear(&p, r, 0, 0);
    mx_gs_finish_ad(&p);
    mx_pkt_end(&p, 0, 0);
    mx_pkt_finish(&p);
    mx_pkt_writeback(&p);
    f0 = mx_gs.finish;
    u32 t0 = mx_count();
    mx_gs_send(pk);
    mx_gs_wait_finish(f0, 1);
    v[r] = mx_count() - t0;
  }
  report("gs: clear 640x224 32-bit + Z (sprite)", v, REPS, "cycles");
}

static void vsync_period(void) {
  u32 v[REPS];
  for (int r = 0; r < REPS; r++) {
    mx_gs_wait_vsync();
    u32 t0 = mx_count();
    mx_gs_wait_vsync();
    v[r] = mx_count() - t0;
  }
  report("gs: vsync period", v, REPS, "cycles");
}

/* Building a packet: 100 A+D register writes with the checked builder. */
static void build_cost(void) {
  u32 v[REPS];
  for (int k = 0; k < 2; k++) {
    for (int r = 0; r < REPS; r++) {
      struct mx_pkt p;
      void *buf = k ? (void *)0x70000000 : (void *)MX_UNCACHED(pk);
      mx_pkt_begin(&p, buf, 4096, 0);
      u32 t0 = mx_count();
      mx_pkt_cnt(&p, 0, 0);
      mx_pkt_ad_begin(&p, 100, 1);
      for (int i = 0; i < 100; i++)
        mx_pkt_ad(&p, MX_GS_REG_RGBAQ, (u64)i);
      mx_pkt_finish(&p);
      v[r] = mx_count() - t0;
    }
    report(k ? "pkt: 100 A+D writes into SPR" : "pkt: 100 A+D writes into uncached", v, REPS,
           "cycles");
  }
}

int main(void) {
  mx_gs_init(640, 224);
  MX_R32(MX_VIF_VIF1_FBRST) = 1;
  struct mx_pkt p;
  mx_pkt_begin(&p, pk, sizeof pk, MX_PKT_VIF);
  mx_pkt_end(&p, 0, 0);
  mx_pkt_mpg(&p, kick_vu, (u32)(kick_vu_end - kick_vu), 0);
  mx_pkt_finish(&p);
  mx_pkt_writeback(&p);
  mx_dma_chain(MX_CH_VIF1, pk, 1);
  mx_dma_wait(MX_CH_VIF1);
  for (u32 i = 0; i < sizeof img / 4; i++)
    img[i] = i;
  mx_dcache_writeback(img, sizeof img);

  mx_printf("p3_gs\n");
  mx_debug_hold(1);
  irq_latency();
  throughput();
  interleave();
  fill();
  vsync_period();
  build_cost();
  mx_debug_hold(0);
  MX_TEST_END(0);
  return 0;
}
