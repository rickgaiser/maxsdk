/* Phase 4 measurements (PS2 only): VU1 batch size vs throughput, UNPACK
   format costs, readback methods (COP2, mapped, toSPR DMA from VU memory),
   T-bit interrupt vs FLUSHE + DMA end, COP2 transfer costs, MPG cost. */

#include <mx/mx.h>

#define REPS 8
#define SPR ((u8 *)0x70000000)
extern const u8 dbl0db_vu[], dbl0db_vu_end[], dbl1db_vu[], dbl1db_vu_end[];
extern const u8 dbl_vu[], dbl_vu_end[], tbit_vu[], tbit_vu_end[], vuend_vu[], vuend_vu_end[];

static float in[4 * 960] MX_ALIGNED(64);
static float out[4 * 960] MX_ALIGNED(64);
static u32 pk[4 * 1100] MX_ALIGNED(64);

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
  mx_printf("%-46s min %7u med %7u %s\n", name, mn, median(v, n), unit);
}

static struct mx_kernel dbl(int vu) {
  struct mx_kernel k = {vu, dbl_vu, (u32)(dbl_vu_end - dbl_vu), 0, 1, 0, 1, 1, 0, 0};
  return k;
}

static void batch_size(void) {
  struct mx_kernel k = dbl(1);
  mx_vu_load(1, k.code, k.code_bytes, 0);
  static struct mx_vuq q;
  mx_vuq_init(&q, 1, 1);
  static const u32 sizes[] = {30, 60, 120, 240, 480, 960};
  u32 hdr[4] MX_ALIGNED(16) = {0};
  for (int s = 0; s < 6; s++) {
    u32 v[REPS];
    for (int r = 0; r < REPS; r++) {
      u32 t0 = mx_count();
      for (u32 done = 0; done < 960; done += sizes[s]) {
        mx_event e = mx_vuq_batch(&q, &k, hdr, &in[4 * done], sizes[s]);
        mx_vuq_wait(&q, e);
        mx_vuq_read(&q, &k, MX_UCAB(&out[4 * done]), sizes[s]);
      }
      v[r] = (mx_count() - t0) * 100 / 960;
    }
    char name[64];
    mx_snprintf(name, sizeof name, "vu1: dbl kernel, batches of %u (send+run+read)", sizes[s]);
    report(name, v, REPS, "cycles/100 qw");
  }
  mx_vuq_release(&q);
}

/* Single vs double-buffered batches of the doubling kernel, 960 items (VU1)
   or 240 (VU0), the next batch sent before the previous one is read. */
static void dbuf_throughput(void) {
  for (int vu = 0; vu < 2; vu++)
    for (int db = 0; db < 2; db++) {
      struct mx_kernel k = {vu, db ? (vu ? dbl1db_vu : dbl0db_vu) : dbl_vu,
                            (u32)(db ? (vu ? dbl1db_vu_end - dbl1db_vu : dbl0db_vu_end - dbl0db_vu)
                                     : dbl_vu_end - dbl_vu),
                            0, 1, 0, 1, 1, db, 0};
      mx_vu_forget(vu);
      mx_vu_load(vu, k.code, k.code_bytes, 0);
      static struct mx_vuq q;
      mx_vuq_init(&q, vu, 1);
      if (vu)
        mx_vuq_readback_spr(&q, SPR, 0x4000);
      u32 total = vu ? 960 : 240, batch = vu ? 120 : 60;
      u32 hdr[4] MX_ALIGNED(16) = {0};
      u32 v[REPS];
      for (int r = 0; r < REPS; r++) {
        u32 t0 = mx_count();
        mx_event prev = {0, 0};
        u32 prev_at = 0;
        for (u32 done = 0; done < total; done += batch) {
          mx_event e = mx_vuq_batch(&q, &k, hdr, &in[4 * done], batch);
          if (!db) {
            mx_vuq_wait(&q, e);
            mx_vuq_read(&q, &k, MX_UCAB(&out[4 * done]), batch);
            continue;
          }
          if (prev.q) {
            mx_vuq_wait(&q, prev);
            mx_vuq_read(&q, &k, MX_UCAB(&out[4 * prev_at]), batch);
          }
          prev = e, prev_at = done;
        }
        if (db) {
          mx_vuq_wait(&q, prev);
          mx_vuq_read(&q, &k, MX_UCAB(&out[4 * prev_at]), batch);
        }
        v[r] = (mx_count() - t0) * 100 / total;
      }
      mx_vuq_release(&q);
      int bad = 0;
      for (u32 i = 0; i < 4 * total; i++)
        bad += MX_UNCACHED(out)[i] != 2 * in[i];
      char name[72];
      mx_snprintf(name, sizeof name, "vu%d: dbl kernel, %s buffer, batches of %u%s", vu,
                  db ? "double" : "single", batch, bad ? " WRONG" : "");
      report(name, v, REPS, "cycles/100 qw");
    }
}

static void unpack_formats(void) {
  static const struct {
    const char *name;
    u32 fmt;
  } f[] = {{"V4-32", MX_UNPACK_V4_32}, {"V3-32", MX_UNPACK_V3_32}, {"V2-16", MX_UNPACK_V2_16},
           {"V4-8", MX_UNPACK_V4_8},   {"V4-5", MX_UNPACK_V4_5},   {"S-32", MX_UNPACK_S_32}};
  for (int i = 0; i < 6; i++) {
    struct mx_pkt p;
    mx_pkt_begin(&p, pk, sizeof pk, MX_PKT_RAW);
    mx_pkt_vif(&p, mx_vif_stcycl(1, 1));
    for (int b = 0; b < 4; b++)
      mx_pkt_unpack(&p, f[i].fmt, (u32)b * 256, 0 /* 256 */, 0, 0);
    int qwc = mx_pkt_finish(&p);
    mx_pkt_writeback(&p);
    u32 v[REPS];
    for (int r = 0; r < REPS; r++) {
      u32 t0 = mx_count();
      mx_dma_normal(MX_CH_VIF1, pk, (u32)qwc);
      mx_dma_wait(MX_CH_VIF1);
      while (MX_R32(MX_VIF_VIF1_STAT) & 0x1F000003u)
        ;
      v[r] = (mx_count() - t0) * 100 / 1024;
    }
    char name[64];
    mx_snprintf(name, sizeof name, "vif1: UNPACK %s, 1024 vectors", f[i].name);
    report(name, v, REPS, "cycles/100 vectors");
  }
}

static void readback(void) {
  u32 v[REPS];
  for (int r = 0; r < REPS; r++) {
    u32 t0 = mx_count();
    mx_vu0_read(MX_UCAB(out), 0, 240);
    v[r] = (mx_count() - t0) * 100 / 240;
  }
  report("vu0: COP2 read (vlqi + sqc2) to UCAB, 240 qw", v, REPS, "cycles/100 qw");
  for (int r = 0; r < REPS; r++) {
    u32 t0 = mx_count();
    mx_vu0_write(0, in, 240);
    v[r] = (mx_count() - t0) * 100 / 240;
  }
  report("vu0: COP2 write (lqc2 + vsqi), 240 qw", v, REPS, "cycles/100 qw");
  for (int r = 0; r < REPS; r++) {
    u32 t0 = mx_count();
    mx_vu1_read(MX_UCAB(out), 0, 960);
    v[r] = (mx_count() - t0) * 100 / 960;
  }
  report("vu1: mapped read (lq + sq) to UCAB, 960 qw", v, REPS, "cycles/100 qw");
  for (int r = 0; r < REPS; r++) {
    u32 t0 = mx_count();
    mx_vu1_read_dma(MX_UCAB(out), 0, 960, SPR, 0x4000);
    v[r] = (mx_count() - t0) * 100 / 960;
  }
  report("vu1: DMA read (toSPR + fromSPR) to UCAB, 960 qw", v, REPS, "cycles/100 qw");
  /* toSPR DMA with VU1 data memory as the source */
  for (int i = 0; i < 16; i++)
    ((volatile u32 *)MX_VU1_DATA)[i] = 0xC0DE0000u + (u32)i; /* stopped VU1: plain stores */
  ((volatile u32 *)SPR)[5] = 0;
  u32 t0 = mx_count();
  MX_DMA_REG(MX_CH_TOSPR, MADR) = MX_VU1_DATA;
  MX_DMA_REG(MX_CH_TOSPR, SADR) = 0;
  MX_DMA_REG(MX_CH_TOSPR, QWC) = 960;
  mx_sync();
  MX_DMA_REG(MX_CH_TOSPR, CHCR) = (u32)MX_CHCR_STR;
  int ok = mx_dma_wait(MX_CH_TOSPR) == 0;
  u32 t = mx_count() - t0;
  mx_printf("vu1: toSPR DMA from VU1 memory, 960 qw: %s, data %s, %u cycles/100 qw\n",
            ok ? "done" : "TIMEOUT", ((volatile u32 *)SPR)[5] == 0xC0DE0005u ? "ok" : "WRONG",
            t * 100 / 960);
}

static volatile u32 t_vu1irq;
static void on_vu1(void *a) { t_vu1irq = mx_count(); }

static void tbit_vs_flushe(void) {
  mx_vu_load(1, tbit_vu, (u32)(tbit_vu_end - tbit_vu), 64);
  mx_vu_load(1, vuend_vu, (u32)(vuend_vu_end - vuend_vu), 72);
  mx_irq_intc(MX_INTC_VU1, on_vu1, 0);
  mx_vu_enable_tbit(1, 1);
  u32 v[REPS], w[REPS];
  for (int r = 0; r < REPS; r++) {
    struct mx_pkt p;
    mx_pkt_begin(&p, MX_UNCACHED(pk), sizeof pk, MX_PKT_VIF);
    mx_pkt_end(&p, mx_vif_mscal(64), 0);
    mx_pkt_finish(&p);
    t_vu1irq = 0;
    u32 t0 = mx_count();
    mx_dma_chain(MX_CH_VIF1, pk, 1);
    while (!t_vu1irq && mx_count() - t0 < 30000000u)
      ;
    v[r] = t_vu1irq ? t_vu1irq - t0 : 0xFFFFFFFFu;
    /* VTS1 stays set after a T-bit stop: reset (which also clears FBRST.TE1) */
    mx_vu_reset(2);
    mx_vu_enable_tbit(1, 1);
    mx_vu_load(1, tbit_vu, (u32)(tbit_vu_end - tbit_vu), 64);
    mx_vu_load(1, vuend_vu, (u32)(vuend_vu_end - vuend_vu), 72);
    mx_pkt_begin(&p, MX_UNCACHED(pk), sizeof pk, MX_PKT_VIF);
    mx_pkt_end(&p, mx_vif_mscal(72), mx_vif_flushe());
    mx_pkt_finish(&p);
    t0 = mx_count();
    mx_dma_chain(MX_CH_VIF1, pk, 1);
    mx_dma_wait(MX_CH_VIF1);
    while ((MX_R32(MX_VIF_VIF1_STAT) & 0x1F000003u) || (mx_vpu_stat() & MX_VPU_VBS1))
      ;
    w[r] = mx_count() - t0;
  }
  mx_vu_enable_tbit(1, 0);
  mx_irq_intc(MX_INTC_VU1, 0, 0);
  report("vu1: MSCAL of a T-bit stop, DMA start to irq", v, REPS, "cycles");
  report("vu1: MSCAL + FLUSHE, DMA start to idle (polled)", w, REPS, "cycles");
}

/* XGKICK placement: 8 sprites as one GIF packet kicked once, or as 8
   packets kicked one by one (kick8.vsm), MSCAL to FINISH. */
extern const u8 kick_vu[], kick_vu_end[], kick8_vu[], kick8_vu_end[];

static void xgkick_placement(void) {
  mx_gs_init(640, 224);
  mx_vu_load(1, kick_vu, (u32)(kick_vu_end - kick_vu), 0);
  mx_vu_load(1, kick8_vu, (u32)(kick8_vu_end - kick8_vu), 16);
  static u64 g[8 * 8 * 2] MX_ALIGNED(16);
  for (int mode = 0; mode < 2; mode++) {
    /* mode 0: one tag with NLOOP 8; mode 1: 8 tags of NLOOP 1, each 4 qw apart */
    u32 qw = 0;
    for (int b = 0; b < 8; b++) {
      if (mode || b == 0) {
        g[2 * qw] = mx_giftag(mode ? 1 : 8, mode ? 1 : 1,
                              (s32)mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 0, 0, 0, 0, 0, 0, 0),
                              MX_GIF_FLG_PACKED, 3);
        g[2 * qw + 1] = mx_gif_regs3(MX_GIF_REG_RGBAQ, MX_GIF_REG_XYZ2, MX_GIF_REG_XYZ2);
        qw++;
      }
      u32 *d = (u32 *)&g[2 * qw];
      d[0] = 0x40, d[1] = 0x40, d[2] = 0x40, d[3] = 0x80;
      d[4] = mx_gs_px(10 + 40 * (u32)b), d[5] = mx_gs_py(10), d[6] = d[7] = 0;
      d[8] = mx_gs_px(40 + 40 * (u32)b), d[9] = mx_gs_py(200), d[10] = d[11] = 0;
      qw += 3;
    }
    u32 v[REPS];
    for (int r = 0; r < REPS; r++) {
      struct mx_pkt p;
      mx_pkt_begin(&p, pk, sizeof pk, MX_PKT_VIF);
      mx_pkt_end(&p, mx_vif_stcycl(1, 1), 0);
      mx_pkt_unpack(&p, MX_UNPACK_V4_32, 0, qw, 0, g);
      mx_pkt_vif(&p, mx_vif_mscal(mode ? 16 : 0));
      mx_pkt_vif(&p, mx_vif_flush());
      mx_pkt_finish(&p);
      mx_pkt_writeback(&p);
      mx_dma_chain(MX_CH_VIF1, pk, 1);
      mx_dma_wait(MX_CH_VIF1);
      while (MX_R32(MX_VIF_VIF1_STAT) & 0x1F000003u)
        ;
      /* only the VU1 start to the end of drawing is timed */
      static u32 f[16] MX_ALIGNED(64);
      mx_pkt_begin(&p, f, sizeof f, 0);
      mx_pkt_end(&p, 0, 0);
      mx_gs_finish_ad(&p);
      mx_pkt_finish(&p);
      mx_pkt_writeback(&p);
      mx_pkt_begin(&p, pk, sizeof pk, MX_PKT_VIF);
      mx_pkt_end(&p, mx_vif_mscal(mode ? 16 : 0), mx_vif_flush());
      mx_pkt_finish(&p);
      mx_pkt_writeback(&p);
      u32 f0 = mx_gs.finish, t0 = mx_count();
      mx_dma_chain(MX_CH_VIF1, pk, 1);
      mx_dma_wait(MX_CH_VIF1);
      mx_gs_send(f);
      mx_gs_wait_finish(f0, 1);
      v[r] = mx_count() - t0;
    }
    report(mode ? "xgkick: 8 sprites, 8 kicks of 1 packet each" : "xgkick: 8 sprites, 1 kick",
           v, REPS, "cycles");
  }
}

static void mpg_cost(void) {
  static u32 code[2 * 1024] MX_ALIGNED(64);
  for (int i = 0; i < 2 * 1024; i++)
    code[i] = (i & 1) ? 0x8000033Cu : 0x000002FFu; /* nop | nop */
  u32 v[REPS];
  for (int r = 0; r < REPS; r++) {
    mx_vu_forget(1);
    u32 t0 = mx_count();
    mx_vu_load(1, code, sizeof code, 1024);
    v[r] = (mx_count() - t0) * 100 / 1024;
  }
  report("vu1: MPG upload of 1024 instructions (ref tags)", v, REPS, "cycles/100 instr");
}

int main(void) {
  mx_dma_init();
  mx_vu_reset(3);
  for (int i = 0; i < 4 * 960; i++)
    in[i] = (float)(i % 97);
  mx_dcache_writeback(in, sizeof in);
  mx_printf("p4_vu\n");
#ifndef PROBE_LIVE
  mx_debug_hold(1); /* no SIF traffic while measuring */
#endif
  batch_size();
  dbuf_throughput();
  unpack_formats();
  readback();
  tbit_vs_flushe();
  xgkick_placement();
  mpg_cost();
  mx_debug_hold(0);
  MX_TEST_END(0);
  return 0;
}
