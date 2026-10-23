/* Phase 5 measurements (PS2 only; PCSX2 runs it for correctness): cursor
   emitters vs the checked builder, fence methods (DMA interrupt, polled VIF
   MARK, GS LABEL), SIGNAL back to back, VU0 readback by DMA, stall control
   with a normal-mode drain, cycle stealing, channels running together, and
   the cost of trace sampling. */

#include <mx/emit.h>
#include <mx/mx.h>

#define REPS 8
#define SPR ((u8 *)0x70000000)
extern const u8 dbl_vu[], dbl_vu_end[];

static u8 big[64 * 1024] MX_ALIGNED(64);
static u32 pk[2048] MX_ALIGNED(64);
static u64 chain[64 * 2 * 18] MX_ALIGNED(64);
static float in[4 * 960] MX_ALIGNED(64);
static float out[4 * 960] MX_ALIGNED(64);

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
  mx_printf("%-50s min %7u med %7u %s\n", name, mn, median(v, n), unit);
}

/* ---- emitters ------------------------------------------------------------ */

/* pcpyld + sq per quadword, for comparison with the emitters' two sd. */
MX_INLINE u64 *qw_sd(u64 *c, u64 lo, u64 hi) {
  u64 t;
  __asm__ volatile("pcpyld %0, %3, %2\n\tsq %0, %1" : "=&r"(t), "=m"(*(u128 *)c) : "r"(lo), "r"(hi));
  return c + 2;
}

static void emit_cost(void) {
  u32 b[REPS], q[REPS], d[REPS], vb[REPS], vq[REPS];
  for (int r = 0; r < REPS; r++) {
    /* 100 A+D writes into the scratchpad */
    struct mx_pkt p;
    u32 t0 = mx_count();
    mx_pkt_begin(&p, SPR, 0x2000, 0);
    mx_pkt_end(&p, 0, 0);
    mx_pkt_ad_begin(&p, 100, 1);
    for (u32 i = 0; i < 100; i++)
      mx_pkt_ad(&p, MX_GS_REG_RGBAQ, i * 0x01010101ull);
    mx_pkt_finish(&p);
    b[r] = (mx_count() - t0) * 100 / 102;

    t0 = mx_count();
    u64 *t = (u64 *)SPR, *c = mx_e_end(t, 0, 0, 0);
    c = mx_e_ad_tag(c, 100, 1);
    for (u32 i = 0; i < 100; i++)
      c = mx_e_ad(c, MX_GS_REG_RGBAQ, i * 0x01010101ull);
    mx_e_close(t, c);
    q[r] = (mx_count() - t0) * 100 / 102;

    t0 = mx_count();
    t = (u64 *)SPR, c = qw_sd(t, mx_dmatag(MX_TAG_END, 0, 0, 0), 0);
    c = qw_sd(c, mx_giftag(100, 1, -1, MX_GIF_FLG_PACKED, 1), MX_GIF_REG_AD);
    for (u32 i = 0; i < 100; i++)
      c = qw_sd(c, i * 0x01010101ull, MX_GS_REG_RGBAQ);
    mx_e_close(t, c);
    d[r] = (mx_count() - t0) * 100 / 102;

    /* 150 vertices, PACKED RGBAQ + XYZ2 (300 qw) */
    t0 = mx_count();
    mx_pkt_begin(&p, SPR, 0x3000, 0);
    mx_pkt_end(&p, 0, 0);
    mx_pkt_giftag(&p, 150, 1, (s32)mx_gs_prim(MX_GS_PRIM_TRIANGLE, 1, 0, 0, 0, 0, 0, 0, 0),
                  MX_GIF_FLG_PACKED, 2, mx_gif_regs2(MX_GIF_REG_RGBAQ, MX_GIF_REG_XYZ2));
    for (u32 i = 0; i < 150; i++) {
      mx_pkt_qw(&p, i | (u64)(i + 1) << 32, (u64)(i + 2) | 0x80ull << 32);
      mx_pkt_qw(&p, (u64)(0x8000 + i * 16) | (u64)(0x8000 + i * 8) << 32, 0x1000);
    }
    mx_pkt_finish(&p);
    vb[r] = (mx_count() - t0) * 100 / 302;

    t0 = mx_count();
    t = (u64 *)SPR, c = mx_e_end(t, 0, 0, 0);
    c = mx_e_giftag(c, 150, 1, (s32)mx_gs_prim(MX_GS_PRIM_TRIANGLE, 1, 0, 0, 0, 0, 0, 0, 0),
                    MX_GIF_FLG_PACKED, 2, mx_gif_regs2(MX_GIF_REG_RGBAQ, MX_GIF_REG_XYZ2));
    for (u32 i = 0; i < 150; i++) {
      c = mx_e_rgbaq(c, i, i + 1, i + 2, 0x80);
      c = mx_e_xyz2(c, 0x8000 + i * 16, 0x8000 + i * 8, 0x1000, 0);
    }
    mx_e_close(t, c);
    vq[r] = (mx_count() - t0) * 100 / 302;
  }
  report("emit: 100 A+D to SPR, checked builder", b, REPS, "cycles/100 qw");
  report("emit: 100 A+D to SPR, emitter (2 x sd)", q, REPS, "cycles/100 qw");
  report("emit: 100 A+D to SPR, pcpyld + sq", d, REPS, "cycles/100 qw");
  report("emit: 150 RGBAQ+XYZ2 vertices, checked builder", vb, REPS, "cycles/100 qw");
  report("emit: 150 RGBAQ+XYZ2 vertices, emitter", vq, REPS, "cycles/100 qw");
}

/* ---- fences --------------------------------------------------------------- */

#define MARK MX_R32(MX_VIF_VIF1_MARK)

/* One VIF1 chain of an end tag with a MARK: from the DMA start until the
   EE sees it, by the DMA end interrupt (mx_dmaq) or by polling VIF1 MARK. */
static void fence_latency(void) {
  u32 irq[REPS], mark[REPS];
  u64 *t = MX_UNCACHED(chain);
  static struct mx_dmaq q;
  mx_dmaq_init(&q, MX_CH_VIF1);
  for (int r = 0; r < REPS; r++) {
    u32 v = (MARK + 1) & 0xFFFF;
    mx_e_end(t, 0, 0, mx_vif_mark(v));
    u32 t0 = mx_count();
    mx_event e = mx_dmaq_chain(&q, chain, 1);
    mx_event_wait(e);
    irq[r] = mx_count() - t0;
    v = (v + 1) & 0xFFFF;
    mx_e_end(t, 0, 0, mx_vif_mark(v));
    mx_dmaq_release(&q);
    t0 = mx_count();
    mx_dma_chain(MX_CH_VIF1, chain, 1);
    while (MARK != v)
      ;
    mark[r] = mx_count() - t0;
    mx_dma_wait(MX_CH_VIF1);
    mx_dmaq_init(&q, MX_CH_VIF1);
  }
  mx_dmaq_release(&q);
  report("fence: VIF1 1-qw chain, DMA end interrupt", irq, REPS, "cycles");
  report("fence: VIF1 1-qw chain, polled VIF MARK", mark, REPS, "cycles");
}

/* 64 batches of 16 VIF NOP quadwords: one queue segment each (an interrupt
   per batch), or one chain with a MARK per batch that the EE polls. */
static void fence_throughput(void) {
  u32 seg[REPS], one[REPS];
  u64 *t = MX_UNCACHED(chain);
  static struct mx_dmaq q;
  for (int r = 0; r < REPS; r++) {
    u32 base = MARK + 1;
    for (int k = 0; k < 64; k++) {
      u64 *c = mx_e_end(t + k * 2 * 17, 16, 0, mx_vif_mark((base + k) & 0xFFFF));
      for (int i = 0; i < 16; i++)
        c = mx_e_qw(c, 0, 0);
    }
    mx_dmaq_init(&q, MX_CH_VIF1);
    u32 t0 = mx_count();
    mx_event e = {0, 0};
    for (int k = 0; k < 64; k++)
      e = mx_dmaq_chain(&q, chain + k * 2 * 17, 1);
    mx_event_wait(e);
    while (MARK != ((base + 63) & 0xFFFF))
      ;
    seg[r] = (mx_count() - t0) / 64;
    mx_dmaq_release(&q);

    base = (base + 64) & 0xFFFF;
    for (int k = 0; k < 64; k++) {
      u64 *c = (k == 63 ? mx_e_end : mx_e_cnt)(t + k * 2 * 17, 16, 0,
                                                mx_vif_mark((base + k) & 0xFFFF));
      for (int i = 0; i < 16; i++)
        c = mx_e_qw(c, 0, 0);
    }
    t0 = mx_count();
    mx_dma_chain(MX_CH_VIF1, chain, 1);
    while (MARK != ((base + 63) & 0xFFFF))
      ;
    one[r] = (mx_count() - t0) / 64;
    mx_dma_wait(MX_CH_VIF1);
  }
  report("fence: 64 x 16 qw, a queue segment per batch", seg, REPS, "cycles/batch");
  report("fence: 64 x 16 qw, one chain, MARK per batch", one, REPS, "cycles/batch");
}

#define SIGLBLID MX_R64(MX_GS_SIGLBLID)
#define CSR MX_R64(MX_GS_CSR)

/* GS: from the GIF DMA start until the EE sees a LABEL (polled). */
static void label_latency(void) {
  u32 v[REPS];
  for (int r = 0; r < REPS; r++) {
    u32 id = 0x5a00 + (u32)r;
    u64 *t = MX_UNCACHED(chain), *c = mx_e_end(t, 0, 0, 0);
    c = mx_e_ad_tag(c, 1, 1);
    c = mx_e_ad(c, MX_GS_REG_LABEL, mx_gs_label(id, 0xFFFFFFFFu));
    mx_e_close(t, c);
    u32 t0 = mx_count();
    mx_dma_chain(MX_CH_GIF, chain, 0);
    while ((u32)(SIGLBLID >> 32) != id)
      ;
    v[r] = mx_count() - t0;
    mx_dma_wait(MX_CH_GIF);
  }
  report("fence: GS LABEL, GIF DMA start to polled", v, REPS, "cycles");
}

/* Two SIGNALs without an acknowledge between them: does the GS stop at the
   second until CSR.SIGNAL is cleared (GS User's Manual)? The GS interrupts
   are masked meanwhile, so our handler does not acknowledge. */
static void signal_back_to_back(void) {
  mx_bios_gs_put_imr(0x7F00);
  CSR = MX_BIT(GS_CSR_SIGNAL) | MX_BIT(GS_CSR_FINISH);
  u64 *t = MX_UNCACHED(chain), *c = mx_e_end(t, 0, 0, 0);
  c = mx_e_ad_tag(c, 4, 1);
  c = mx_e_ad(c, MX_GS_REG_LABEL, mx_gs_label(0, 0xFFFFFFFFu));
  c = mx_e_ad(c, MX_GS_REG_SIGNAL, mx_gs_signal(0x11, 0xFFFFFFFFu));
  c = mx_e_ad(c, MX_GS_REG_SIGNAL, mx_gs_signal(0x22, 0xFFFFFFFFu));
  c = mx_e_ad(c, MX_GS_REG_LABEL, mx_gs_label(0x33, 0xFFFFFFFFu));
  mx_e_close(t, c);
  mx_dma_chain(MX_CH_GIF, chain, 0);
  mx_delay_us(500);
  u64 s1 = SIGLBLID, c1 = CSR;
  int dma1 = mx_dma_busy(MX_CH_GIF);
  CSR = MX_BIT(GS_CSR_SIGNAL); /* acknowledge */
  u32 t0 = mx_count();
  while ((u32)(SIGLBLID >> 32) != 0x33 && mx_count() - t0 < 1000000)
    ;
  u32 resume = mx_count() - t0;
  u64 s2 = SIGLBLID, c2 = CSR;
  mx_printf("signal: before ack: SIGID %x LBLID %x CSR.SIGNAL %u GIF DMA %s\n", (u32)s1,
            (u32)(s1 >> 32), (u32)(c1 & 1), dma1 ? "busy" : "done");
  mx_printf("signal: after ack:  SIGID %x LBLID %x CSR.SIGNAL %u, %u cycles to the LABEL\n",
            (u32)s2, (u32)(s2 >> 32), (u32)(c2 & 1), resume);
  mx_dma_wait(MX_CH_GIF);
  CSR = MX_BIT(GS_CSR_SIGNAL) | MX_BIT(GS_CSR_FINISH);
  mx_bios_gs_put_imr(0x7C00);
}

/* ---- VU0 readback ------------------------------------------------------- */

static void vu0_readback(void) {
  struct mx_kernel k = {0, dbl_vu, (u32)(dbl_vu_end - dbl_vu), 0, 1, 0, 1, 1, 0, 0};
  mx_vu_forget(0);
  mx_vu_load(0, k.code, k.code_bytes, 0);
  for (int spr = 0; spr < 2; spr++) {
    static struct mx_vuq q;
    mx_vuq_init(&q, 0, 1);
    if (spr)
      mx_vuq_readback_spr(&q, SPR, 0x4000);
    u32 hdr[4] MX_ALIGNED(16) = {0};
    u32 v[REPS];
    for (int r = 0; r < REPS; r++) {
      u32 t0 = mx_count();
      for (u32 done = 0; done < 960; done += 240) {
        mx_event e = mx_vuq_batch(&q, &k, hdr, &in[4 * done], 240);
        mx_vuq_wait(&q, e);
        mx_vuq_read(&q, &k, MX_UCAB(&out[4 * done]), 240);
      }
      v[r] = (mx_count() - t0) * 100 / 960;
    }
    mx_vuq_release(&q);
    int bad = 0;
    for (int i = 0; i < 4 * 960; i++)
      bad += MX_UNCACHED(out)[i] != 2 * in[i];
    report(spr ? "vu0: dbl, VIF0 in, readback by toSPR DMA" : "vu0: dbl, VIF0 in, readback by COP2",
           v, REPS, bad ? "cycles/100 qw WRONG" : "cycles/100 qw");
  }
}

/* ---- DMAC ------------------------------------------------------------------ */

#define VU1_MEM ((volatile u32 *)MX_VU1_DATA)

/* Stall control with the drain (VIF1) in normal mode: it must not read the
   buffer before fromSPR has written it. PCSX2 does not stall it. */
static void stall_normal(void) {
  u32 *buf = (u32 *)big;
  for (int i = 0; i < 32; i++)
    MX_UNCACHED(buf)[i] = 0;
  for (int w = 0; w < 28; w++)
    VU1_MEM[300 * 4 + w] = 0;
  volatile u32 *s = (volatile u32 *)(SPR + 0x3000);
  s[0] = 0;
  s[1] = mx_vif_stcycl(1, 1);
  s[2] = 0;
  s[3] = mx_vif_unpack(MX_UNPACK_V4_32, 300, 7, 0);
  for (int w = 0; w < 28; w++)
    s[4 + w] = 0x70000000u | (u32)w;
  mx_dma_stall(MX_STALL_SRC_FROMSPR, MX_STALL_DRAIN_VIF1, buf);
  mx_dma_normal(MX_CH_VIF1, buf, 8);
  mx_delay_us(100);
  int early = mx_dma_busy(MX_CH_VIF1);
  mx_dma_from_spr(buf, (const void *)s, 8);
  mx_dma_wait(MX_CH_FROMSPR);
  mx_dma_wait(MX_CH_VIF1);
  mx_dma_stall(0, 0, 0);
  int bad = 0;
  for (int w = 0; w < 28; w++)
    bad += VU1_MEM[300 * 4 + w] != (0x70000000u | (u32)w);
  mx_printf("stall: normal-mode VIF1 drain %s before the source wrote; %d of 28 words wrong\n",
            early ? "waited" : "did not wait", bad);
}

/* 2048 uncached stores by the EE while VIF1 reads 16 KiB from memory. */
static u32 ee_stores(void) {
  volatile u32 *p = MX_UNCACHED((u32 *)(big + 32 * 1024));
  u32 t0 = mx_count();
  for (int i = 0; i < 2048; i++)
    p[i & 1023] = (u32)i;
  return mx_count() - t0;
}

static void cycle_steal(void) {
  for (int i = 0; i < 16 * 1024 / 4; i++)
    MX_UNCACHED((u32 *)big)[i] = 0;
  static const struct {
    const char *name;
    u32 ctrl; /* RELE, RCYC */
  } modes[] = {{"no release", 0}, {"RELE, 8 cycles", 1}, {"RELE, 64 cycles", 3 << 7 | 1}};
  u32 alone[REPS];
  for (int r = 0; r < REPS; r++)
    alone[r] = ee_stores();
  report("dmac: 2048 uncached EE stores alone", alone, REPS, "cycles");
  u32 c0 = MX_R32(MX_D_CTRL);
  for (int m = 0; m < 3; m++) {
    u32 rcyc = modes[m].ctrl >> 7;
    MX_R32(MX_D_CTRL) = (c0 & ~(u32)(MX_BIT(D_CTRL_RELE) | MX_BIT(D_CTRL_RCYC))) |
                        (u32)(MX_SET(D_CTRL_RELE, modes[m].ctrl & 1) | MX_SET(D_CTRL_RCYC, rcyc));
    u32 ee[REPS], dma[REPS];
    for (int r = 0; r < REPS; r++) {
      u32 t0 = mx_count();
      mx_dma_normal(MX_CH_VIF1, big, 1024);
      ee[r] = ee_stores();
      mx_dma_wait(MX_CH_VIF1);
      dma[r] = mx_count() - t0;
    }
    char name[64];
    mx_snprintf(name, sizeof name, "dmac: EE stores during VIF1 16 KiB, %s", modes[m].name);
    report(name, ee, REPS, "cycles");
    mx_snprintf(name, sizeof name, "dmac: VIF1 16 KiB with the stores, %s", modes[m].name);
    report(name, dma, REPS, "cycles");
  }
  MX_R32(MX_D_CTRL) = c0;
}

/* toSPR 16 KiB, VIF1 16 KiB and a GIF 64 KiB IMAGE upload: each alone, and
   all three started together (end time of each). */
static void together(void) {
  struct mx_pkt p;
  mx_pkt_begin(&p, pk, sizeof pk, 0);
  mx_pkt_cnt(&p, 0, 0);
  mx_gs_upload(&p, mx_gs.tbp, 4, MX_GS_PSM_CT32, 0, 0, 256, 64, big);
  mx_pkt_end(&p, 0, 0);
  mx_pkt_finish(&p);
  mx_pkt_writeback(&p);
  static const u8 chs[3] = {MX_CH_TOSPR, MX_CH_VIF1, MX_CH_GIF};
  static const char *names[3] = {"toSPR 16 KiB", "VIF1 16 KiB", "GIF 64 KiB IMAGE"};
  u32 alone[3][REPS], both[3][REPS];
  for (int r = 0; r < REPS; r++) {
    for (int k = 0; k < 4; k++) {
      u32 t0 = mx_count();
      if (k == 0 || k == 3)
        mx_dma_to_spr(SPR, big + 16 * 1024, 1024);
      if (k == 1 || k == 3)
        mx_dma_normal(MX_CH_VIF1, big, 1024);
      if (k == 2 || k == 3)
        mx_dma_chain(MX_CH_GIF, pk, 0);
      u32 end[3] = {0, 0, 0}, left = k == 3 ? 3 : 1;
      while (left)
        for (int c = 0; c < 3; c++)
          if ((k == 3 || k == c) && !end[c] && !mx_dma_busy(chs[c])) {
            end[c] = mx_count() - t0;
            left--;
          }
      for (int c = 0; c < 3; c++) {
        if (k == c)
          alone[c][r] = end[c];
        if (k == 3)
          both[c][r] = end[c];
      }
    }
  }
  for (int c = 0; c < 3; c++) {
    char name[64];
    mx_snprintf(name, sizeof name, "dmac: %s alone", names[c]);
    report(name, alone[c], REPS, "cycles");
    mx_snprintf(name, sizeof name, "dmac: %s, all three started together", names[c]);
    report(name, both[c], REPS, "cycles (end)");
  }
}

/* ---- trace sampling --------------------------------------------------------- */

static volatile u32 sink;
static u32 workload(void) {
  u32 t0 = mx_count(), x = 1;
  for (int i = 0; i < 16000000; i++)
    x = x * 3 + 1;
  sink = x;
  return mx_count() - t0;
}

static void sampling(void) {
  u32 base = workload();
  static const u32 hz[2] = {4000, 16000};
  for (int h = 0; h < 2; h++) {
    mx_trace_sample(hz[h]);
    u32 t = workload();
    u32 busy[MX_TS_N];
    u32 n = mx_trace_busy(busy);
    mx_trace_sample(0);
    mx_printf("sample: %u Hz: %u samples, workload %u -> %u cycles, %u cycles per sample\n",
              hz[h], n, base, t, n ? (t - base) / n : 0);
  }
  /* VIF1 kept busy for ~10 ms: the samples should see it */
  for (u32 i = 0; i < sizeof big / 4; i++)
    MX_UNCACHED((u32 *)big)[i] = 0; /* VIF NOPs */
  mx_trace_sample(16000);
  u32 t0 = mx_count();
  while (mx_count() - t0 < MX_EE_HZ / 100) {
    mx_dma_normal(MX_CH_VIF1, big, 4096);
    mx_dma_wait(MX_CH_VIF1);
  }
  u32 busy[MX_TS_N];
  u32 n = mx_trace_busy(busy);
  mx_trace_sample(0);
  mx_printf("sample: VIF1 DMA loop: %u samples, VIF1 DMA busy %u%%, GIF %u%%, VU1 %u%%\n", n,
            n ? busy[1] * 100 / n : 0, n ? busy[2] * 100 / n : 0, n ? busy[6] * 100 / n : 0);
}

int main(void) {
  mx_gs_init(640, 224);
  mx_vu_reset(3);
  for (int i = 0; i < 4 * 960; i++)
    in[i] = (float)(i % 97);
  mx_dcache_writeback(in, sizeof in);
  mx_printf("p5_fast\n");
#ifndef PROBE_LIVE
  mx_debug_hold(1); /* no SIF traffic while measuring */
#endif
  emit_cost();
  fence_latency();
  fence_throughput();
  label_latency();
  signal_back_to_back();
  vu0_readback();
  stall_normal();
  cycle_steal();
  together();
  sampling();
  mx_debug_hold(0);
  MX_TEST_END(0);
  return 0;
}
