/* Phase 2 measurements (PS2 only; PCSX2 timings are meaningless):
   interrupt latency, DMA engine restart gap, cache write back, store speed
   per memory type, DMA throughput per channel, MFIFO, and the VIF0
   uncached-tag case. Prints "name value unit" lines for docs/. */

#include <mx/mx.h>

#define SPR ((u8 *)0x70000000)
#define REPS 16
#define VU0_MEM ((volatile u32 *)MX_VU0_DATA)

static u8 big[64 * 1024] MX_ALIGNED(64);
static u64 tags[64 * 2] MX_ALIGNED(64);
static u8 ring[8192] MX_ALIGNED(8192);

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
  mx_printf("%-40s min %6u med %6u %s\n", name, mn, median(v, n), unit);
}

/* ---- interrupt latency --------------------------------------------------- */

static volatile u32 t_irq;
static void on_end(void *a) { t_irq = mx_count(); }

static void irq_latency(void) {
  u32 lat[REPS], poll[REPS];
  mx_irq_dmac(MX_CH_TOSPR, on_end, 0);
  for (int r = 0; r < REPS; r++) {
    t_irq = 0;
    u32 t0 = mx_count();
    mx_dma_to_spr(SPR, big, 1);
    while (!t_irq)
      ;
    lat[r] = t_irq - t0;
  }
  mx_irq_dmac(MX_CH_TOSPR, 0, 0);
  for (int r = 0; r < REPS; r++) {
    u32 t0 = mx_count();
    mx_dma_to_spr(SPR, big, 1);
    while (mx_dma_busy(MX_CH_TOSPR))
      ;
    poll[r] = mx_count() - t0;
  }
  report("irq: toSPR 1 qw, start to handler", lat, REPS, "cycles");
  report("irq: toSPR 1 qw, start to polled end", poll, REPS, "cycles");
}

/* ---- DMA engine restart gap ---------------------------------------------- */

#define NSEG 32
#define SEGQ 16
static void engine_gap(void) {
  /* VIF NOPs: VIF1 consumes them quickly */
  memset(big, 0, NSEG * SEGQ * 16);
  mx_dcache_writeback(big, NSEG * SEGQ * 16);
  u32 single[REPS], queued[REPS], swgap[REPS];
  for (int r = 0; r < REPS; r++) {
    u32 t0 = mx_count();
    mx_dma_normal(MX_CH_VIF1, big, NSEG * SEGQ);
    mx_dma_wait(MX_CH_VIF1);
    single[r] = mx_count() - t0;
  }
  struct mx_dmaq q;
  mx_dmaq_init(&q, MX_CH_VIF1);
  for (int r = 0; r < REPS; r++) {
    mx_trace_clear();
    mx_trace_enable(1);
    u32 t0 = mx_count();
    mx_event e = {0, 0};
    for (int i = 0; i < NSEG; i++)
      e = mx_dmaq_normal(&q, big + i * SEGQ * 16, SEGQ);
    mx_event_wait(e);
    queued[r] = mx_count() - t0;
    mx_trace_enable(0);
    /* software part of the gap: handler entry to the next start */
    static struct mx_trace_ev ev[512];
    u32 n = mx_trace_get(ev, 512), best = 0xFFFFFFFFu;
    for (u32 i = 0; i + 1 < n; i++)
      if (ev[i].ev == MX_TR_DMAC_IRQ && ev[i + 1].ev == MX_TR_DMA_START)
        if (ev[i + 1].t - ev[i].t < best)
          best = ev[i + 1].t - ev[i].t;
    swgap[r] = best;
  }
  mx_dmaq_release(&q);
  report("vif1: 512 qw NOP, one normal transfer", single, REPS, "cycles");
  report("vif1: 32 x 16 qw NOP through the engine", queued, REPS, "cycles");
  u32 per[REPS];
  for (int r = 0; r < REPS; r++)
    per[r] = queued[r] > single[r] ? (queued[r] - single[r]) / (NSEG - 1) : 0;
  report("engine: extra cycles per segment", per, REPS, "cycles");
  report("engine: handler entry to next start", swgap, REPS, "cycles");
}

/* ---- cache write back ---------------------------------------------------- */

static void cache_wb(void) {
  static const u32 sizes[] = {1024, 4096, 16384};
  for (int s = 0; s < 3; s++) {
    u32 v[REPS];
    for (int r = 0; r < REPS; r++) {
      memset(big, r, sizes[s]);
      u32 t0 = mx_count();
      mx_dcache_writeback(big, sizes[s]);
      v[r] = mx_count() - t0;
    }
    char name[64];
    mx_snprintf(name, sizeof name, "cache: writeback %u B dirty", sizes[s]);
    report(name, v, REPS, "cycles");
  }
  u32 v[REPS];
  for (int r = 0; r < REPS; r++) {
    memset(big, r, 16384);
    u32 t0 = mx_count();
    mx_bios_flush_cache(0);
    v[r] = mx_count() - t0;
  }
  report("cache: BIOS FlushCache(0), 16 KiB dirty", v, REPS, "cycles");
}

/* ---- store speed per memory type ----------------------------------------- */

static u32 store_sq(void *p, u32 bytes) {
  u32 t0 = mx_count();
  __asm__ volatile(".set push\n\t.set noreorder\n\t"
                   "addu $9, %0, %1\n"
                   "1:\tsq $0, 0(%0)\n\t"
                   "sq $0, 16(%0)\n\t"
                   "sq $0, 32(%0)\n\t"
                   "sq $0, 48(%0)\n\t"
                   "addiu %0, %0, 64\n\t"
                   "bne %0, $9, 1b\n\t"
                   "nop\n\t"
                   "sync.l\n\t"
                   ".set pop"
                   : "+r"(p)
                   : "r"(bytes)
                   : "$9", "memory");
  return mx_count() - t0;
}

static void store_speed(void) {
  struct {
    const char *name;
    void *p;
    u32 bytes;
  } m[] = {
      {"store: sq to cached memory (hot)", big, 8192},
      {"store: sq to uncached memory", MX_UNCACHED(big), 8192},
      {"store: sq to UCAB memory", MX_UCAB(big), 8192},
      {"store: sq to scratchpad", SPR, 8192},
  };
  for (int k = 0; k < 4; k++) {
    u32 v[REPS];
    for (int r = 0; r < REPS; r++)
      v[r] = store_sq(m[k].p, m[k].bytes) * 100 / (m[k].bytes / 16);
    report(m[k].name, v, REPS, "cycles/100 qw");
  }
  mx_bios_flush_cache(0);
}

/* ---- DMA throughput ------------------------------------------------------ */

static void throughput(void) {
  memset(big, 0, sizeof big);
  mx_bios_flush_cache(0);
  u32 v[REPS];
  for (int r = 0; r < REPS; r++) {
    u32 t0 = mx_count();
    mx_dma_to_spr(SPR, big, 1024);
    mx_dma_wait(MX_CH_TOSPR);
    v[r] = (mx_count() - t0) * 100 / 1024;
  }
  report("dma: toSPR 16 KiB from memory", v, REPS, "cycles/100 qw");
  for (int r = 0; r < REPS; r++) {
    u32 t0 = mx_count();
    mx_dma_from_spr(big, SPR, 1024);
    mx_dma_wait(MX_CH_FROMSPR);
    v[r] = (mx_count() - t0) * 100 / 1024;
  }
  report("dma: fromSPR 16 KiB to memory", v, REPS, "cycles/100 qw");
  memset(big, 0, 16384);
  mx_bios_flush_cache(0);
  for (int ch = 0; ch < 2; ch++) {
    for (int r = 0; r < REPS; r++) {
      u32 t0 = mx_count();
      mx_dma_normal(ch, big, 1024);
      mx_dma_wait(ch);
      v[r] = (mx_count() - t0) * 100 / 1024;
    }
    report(ch ? "dma: VIF1 16 KiB VIF NOPs from memory" : "dma: VIF0 16 KiB VIF NOPs from memory",
           v, REPS, "cycles/100 qw");
  }
  /* VIF1 from SPR */
  memset(SPR, 0, 16384);
  for (int r = 0; r < REPS; r++) {
    u32 t0 = mx_count();
    mx_dma_normal(MX_CH_VIF1, SPR, 1024);
    mx_dma_wait(MX_CH_VIF1);
    v[r] = (mx_count() - t0) * 100 / 1024;
  }
  report("dma: VIF1 16 KiB VIF NOPs from SPR", v, REPS, "cycles/100 qw");
}

/* ---- MFIFO --------------------------------------------------------------- */

static void mfifo(void) {
  /* 8 packets of a cnt tag + 63 qw of VIF NOPs in SPR, pushed 16 times each */
  volatile u64 *s = (volatile u64 *)SPR;
  for (int p = 0; p < 8; p++) {
    s[p * 128] = mx_dmatag(MX_TAG_CNT, 63, 0, 0);
    for (int i = 1; i < 128; i++)
      s[p * 128 + i] = 0;
  }
  u32 v[REPS];
  for (int r = 0; r < REPS; r++) {
    mx_mfifo_init(MX_CH_VIF1, ring, sizeof ring, 0);
    u32 t0 = mx_count();
    for (int k = 0; k < 16; k++)
      for (int p = 0; p < 8; p++)
        mx_mfifo_push((const void *)&s[p * 128], 64);
    while (mx_mfifo_used())
      ;
    v[r] = (mx_count() - t0) * 100 / (16 * 8 * 64);
    mx_mfifo_stop();
  }
  report("mfifo: fromSPR -> ring -> VIF1, 8 KiB ring", v, REPS, "cycles/100 qw");
}

/* ---- VIF0 with tags written uncached --------------------------------------- */

static void vif0_uncached_tags(void) {
  static u32 data[4] MX_ALIGNED(64) = {0x11111111, 0x22222222, 0x33333333, 0x44444444};
  mx_dcache_writeback(data, 16);
  for (int mode = 0; mode < 2; mode++) {
    u64 *t = mode ? MX_UNCACHED(tags) : tags;
    VU0_MEM[4 * 20] = 0;
    t[0] = mx_dmatag(MX_TAG_REF, 1, data, 0);
    t[1] = 0x01000101u | (u64)(0x6C010000u | 20) << 32;
    t[2] = mx_dmatag(MX_TAG_END, 0, 0, 0);
    t[3] = 0;
    if (!mode)
      mx_dcache_writeback(tags, 32);
    mx_dma_chain(MX_CH_VIF0, t, 1);
    int ok = mx_dma_wait(MX_CH_VIF0) == 0;
    mx_printf("vif0: chain, tags written %-8s started %d, data %s, CHCR %08x TADR %08x\n",
              mode ? "uncached" : "cached", ok, VU0_MEM[4 * 20] == 0x11111111u ? "ok" : "MISSING",
              MX_DMA_REG(MX_CH_VIF0, CHCR), MX_DMA_REG(MX_CH_VIF0, TADR));
    if (!ok)
      MX_DMA_REG(MX_CH_VIF0, CHCR) = 0;
  }
}

int main(void) {
  mx_dma_init();
  MX_R32(MX_VIF_VIF0_FBRST) = 1;
  MX_R32(MX_VIF_VIF1_FBRST) = 1;
  mx_printf("p2_dma: machine type %d, memory %d\n", mx_bios_machine_type(),
            mx_bios_get_memory_size());
  mx_debug_hold(1);
  irq_latency();
  engine_gap();
  cache_wb();
  store_speed();
  throughput();
  mfifo();
  vif0_uncached_tags();
  mx_debug_hold(0);
  MX_TEST_END(0);
  return 0;
}
