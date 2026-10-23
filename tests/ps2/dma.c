/* Phase 2: interrupts, every DMA mode, MFIFO, the DMA engine and timers. */

#include "t.h"

#ifndef MX_DEBUG_SIO
#define MX_DEBUG_SIO 0
#endif

#define SPR ((u8 *)0x70000000)
#define VU0_MEM ((volatile u32 *)MX_VU0_DATA)
#define VU1_MEM ((volatile u32 *)MX_VU1_DATA)

#define STCYCL_1_1 0x01000101u
#define UNPACK_V4_32(n, addr) (0x6C000000u | ((u32)(n) << 16) | (u32)(addr))

static u32 buf[256] MX_ALIGNED(64);
static u32 buf2[256] MX_ALIGNED(64);
static u64 tags[2 * 260] MX_ALIGNED(64);
static u32 data[4 * 64] MX_ALIGNED(64);
static u8 ring[4096] MX_ALIGNED(4096);

static void fill(u32 *p, int n, u32 seed) {
  for (int i = 0; i < n; i++)
    p[i] = seed + (u32)i * 0x01010101u;
}

static void spr_normal(void) {
  fill(buf, 256, 0x10000000);
  mx_dcache_writeback(buf, sizeof buf);
  mx_dma_to_spr(SPR, buf, 64);
  CHECK(mx_dma_wait(MX_CH_TOSPR) == 0);
  CHECK_EQ(((volatile u32 *)SPR)[37], buf[37]);
  memset(buf2, 0, sizeof buf2);
  mx_dcache_writeback(buf2, sizeof buf2);
  mx_dma_from_spr(buf2, SPR, 64);
  CHECK(mx_dma_wait(MX_CH_FROMSPR) == 0);
  mx_dcache_invalidate(buf2, sizeof buf2);
  CHECK(memcmp(buf, buf2, sizeof buf) == 0);
}

static void spr_interleave(void) {
  /* take 2 of every 4 quadwords: 32 quadwords out of 64 */
  fill(buf, 256, 0x20000000);
  mx_dcache_writeback(buf, sizeof buf);
  mx_dma_to_spr_interleave(SPR + 0x1000, buf, 32, 2, 2);
  CHECK(mx_dma_wait(MX_CH_TOSPR) == 0);
  volatile u32 *s = (volatile u32 *)(SPR + 0x1000);
  int ok = 1;
  for (int q = 0; q < 32; q++) {
    int src = (q / 2) * 4 + q % 2;
    for (int w = 0; w < 4; w++)
      ok &= s[q * 4 + w] == buf[src * 4 + w];
  }
  CHECK(ok);
}

static void spr_dest_chain(void) {
  /* fromSPR destination chain: cnt tag to buf2 (2 qw), end tag to buf2+64 (1 qw) */
  volatile u64 *s = (volatile u64 *)(SPR + 0x2000);
  memset(buf2, 0, sizeof buf2);
  mx_dcache_writeback(buf2, sizeof buf2);
  s[0] = mx_dmatag(MX_TAG_CNT, 2, buf2, 0);
  s[1] = 0;
  for (int i = 0; i < 4; i++)
    s[2 + i] = 0xC0DE000000000000ull + (u64)i;
  s[6] = mx_dmatag(MX_TAG_END, 1, &buf2[64], 0);
  s[7] = 0;
  s[8] = 0x1111222233334444ull;
  s[9] = 0x5555666677778888ull;
  mx_dma_from_spr_chain((const void *)s);
  CHECK(mx_dma_wait(MX_CH_FROMSPR) == 0);
  mx_dcache_invalidate(buf2, sizeof buf2);
  CHECK_EQ(((u64 *)buf2)[3], 0xC0DE000000000003ull);
  CHECK_EQ(((u64 *)buf2)[32], 0x1111222233334444ull);
  CHECK_EQ(((u64 *)buf2)[33], 0x5555666677778888ull);
}

/* REF tag to n quadwords of data, with STCYCL + UNPACK to addr in the tag. */
static int ref_unpack(u64 *t, const void *d, int n, int addr) {
  t[0] = mx_dmatag(MX_TAG_REF, (u32)n, d, 0);
  t[1] = STCYCL_1_1 | (u64)UNPACK_V4_32(n, addr) << 32;
  return 2;
}

static void vif_chain(int ch, volatile u32 *vumem, int addr) {
  fill(data, 16, 0x30000000u + (u32)ch);
  mx_dcache_writeback(data, sizeof data);
  int k = ref_unpack(tags, data, 4, addr);
  tags[k] = mx_dmatag(MX_TAG_END, 0, 0, 0);
  tags[k + 1] = 0;
  mx_dcache_writeback(tags, sizeof tags);
  mx_dma_chain(ch, tags, 1);
  CHECK(mx_dma_wait(ch) == 0);
  for (int i = 0; i < 16; i++)
    if (vumem[addr * 4 + i] != data[i]) {
      mx_printf("vif%d chain: VU mem word %d = %08x, expected %08x\n", ch, i, vumem[addr * 4 + i],
                data[i]);
      t_fail++;
      break;
    }
}

static void vif_normal(int ch, volatile u32 *vumem, int addr) {
  u32 *p = buf;
  p[0] = 0;
  p[1] = STCYCL_1_1;
  p[2] = 0;
  p[3] = UNPACK_V4_32(2, addr);
  fill(p + 4, 8, 0x40000000u + (u32)ch);
  mx_dcache_writeback(buf, 64);
  mx_dma_normal(ch, buf, 3);
  CHECK(mx_dma_wait(ch) == 0);
  CHECK_EQ(vumem[addr * 4 + 7], p[4 + 7]);
}

/* Many small segments through the engine; each unpacks one marker quadword. */
#define NSEG 48
static void engine(int ch, volatile u32 *vumem) {
  struct mx_dmaq q;
  mx_dmaq_init(&q, ch);
  u32 irq0 = mx_irq_dmac_count(ch);
  for (int i = 0; i < NSEG; i++)
    for (int w = 0; w < 4; w++)
      data[i * 4 + w] = 0x50000000u | (u32)ch << 16 | (u32)i << 4 | (u32)w;
  mx_dcache_writeback(data, sizeof data);
  mx_event last = {0, 0};
  for (int i = 0; i < NSEG; i++) {
    u64 *t = &tags[i * 4];
    ref_unpack(t, &data[i * 4], 1, 100 + i);
    t[2] = mx_dmatag(MX_TAG_END, 0, 0, 0);
    t[3] = 0;
    mx_dcache_writeback(t, 32);
    last = mx_dmaq_chain(&q, t, 1);
  }
  u32 t0 = mx_count();
  while (!mx_event_done(last) && mx_count() - t0 < 300000000u)
    ;
  if (!mx_event_done(last))
    mx_printf("engine ch%d stuck: done %u head %u tail %u irqs %u CHCR %08x D_STAT %08x\n", ch,
              q.done, q.head, q.tail, mx_irq_dmac_count(ch) - irq0, MX_DMA_REG(ch, CHCR),
              MX_R32(MX_D_STAT));
  CHECK_EQ(q.done, NSEG);
  CHECK_EQ(mx_irq_dmac_count(ch) - irq0, NSEG);
  int bad = 0;
  for (int i = 0; i < NSEG; i++)
    for (int w = 0; w < 4; w++)
      bad += vumem[(100 + i) * 4 + w] != data[i * 4 + w];
  CHECK_EQ(bad, 0);
  mx_dmaq_release(&q);
}

static void mfifo(void) {
  mx_mfifo_init(MX_CH_VIF1, ring, sizeof ring);
  /* 24 packets of cnt tag + [NOP STCYCL NOP UNPACK] + 2 quadwords, built in
     two SPR halves: a half is only rewritten after its push has finished
     (on the PS2 the copy runs while the next packet is built) */
  for (int i = 0; i < 24; i++) {
    volatile u32 *s = (volatile u32 *)(SPR + (i & 1) * 0x2000);
    if (i >= 2)
      mx_mfifo_push_wait();
    u64 tag = mx_dmatag(MX_TAG_CNT, 3, 0, 0);
    s[0] = (u32)tag;
    s[1] = (u32)(tag >> 32);
    s[2] = s[3] = 0;
    s[4] = 0;
    s[5] = STCYCL_1_1;
    s[6] = 0;
    s[7] = UNPACK_V4_32(2, 200 + 2 * i);
    for (int w = 0; w < 8; w++)
      s[8 + w] = 0x60000000u | (u32)i << 8 | (u32)w;
    mx_mfifo_push((const void *)s, 4);
  }
  u32 t0 = mx_count();
  while (mx_mfifo_used() && mx_count() - t0 < 100000000u)
    ;
  CHECK_EQ(mx_mfifo_used(), 0);
  int bad = 0;
  for (int i = 0; i < 24; i++)
    for (int w = 0; w < 8; w++)
      bad += VU1_MEM[(200 + 2 * i) * 4 + w] != (0x60000000u | (u32)i << 8 | (u32)w);
  CHECK_EQ(bad, 0);
  mx_mfifo_stop();
  CHECK(!mx_dma_busy(MX_CH_VIF1));
}

/* Stall control: VIF1 (drain, a refs packet) reads buf only after fromSPR
   (source) has written it. */
static void stall(void) {
  memset(buf, 0, sizeof buf);
  mx_dcache_writeback(buf, sizeof buf);
  volatile u32 *s = (volatile u32 *)(SPR + 0x3000);
  s[0] = 0;
  s[1] = STCYCL_1_1;
  s[2] = 0;
  s[3] = UNPACK_V4_32(7, 300);
  for (int w = 0; w < 28; w++)
    s[4 + w] = 0x70000000u | (u32)w;
  mx_dma_stall(MX_STALL_SRC_FROMSPR, MX_STALL_DRAIN_VIF1, buf);
  /* drain in source chain mode: refs packets are stall controlled */
  tags[0] = mx_dmatag(MX_TAG_REFS, 8, buf, 0);
  tags[1] = 0;
  tags[2] = mx_dmatag(MX_TAG_END, 0, 0, 0);
  tags[3] = 0;
  mx_dcache_writeback(tags, 32);
  mx_dma_chain(MX_CH_VIF1, tags, 0); /* stalls: nothing written yet */
  mx_delay_us(100);
  CHECK(VU1_MEM[300 * 4] != 0x70000000u);
  mx_dma_from_spr(buf, (const void *)s, 8);
  CHECK(mx_dma_wait(MX_CH_FROMSPR) == 0);
  CHECK(mx_dma_wait(MX_CH_VIF1) == 0);
  mx_dma_stall(0, 0, 0);
  int bad = 0;
  for (int w = 0; w < 28; w++)
    bad += VU1_MEM[300 * 4 + w] != (0x70000000u | (u32)w);
  CHECK_EQ(bad, 0);
}

static volatile u32 ticks;
static void tick(void *arg) { ticks++; }

static void timer(void) {
  /* 147.456 MHz / 256 = 576 kHz; period 576 -> 1 kHz */
  ticks = 0;
  mx_timer_periodic(1, 576, tick, 0);
  mx_delay_us(50000);
  mx_timer_periodic(1, 0, 0, 0);
  u32 n = ticks;
  mx_printf("timer: %u ticks in 50 ms\n", n);
  CHECK(n >= 40 && n <= 60);
}

#define STEP(x)                                                                            \
  do {                                                                                     \
    mx_printf("- %s\n", #x);                                                               \
    x;                                                                                     \
  } while (0)

int main(void) {
  if (MX_DEBUG_SIO)
    mx_debug_sinks(MX_SINK_SIO);
  mx_dma_init();
  MX_R32(MX_VIF_VIF0_FBRST) = 1; /* reset the VIFs */
  MX_R32(MX_VIF_VIF1_FBRST) = 1;
  STEP(spr_normal());
  STEP(spr_interleave());
  STEP(spr_dest_chain());
  STEP(vif_normal(MX_CH_VIF0, VU0_MEM, 2));
  STEP(vif_normal(MX_CH_VIF1, VU1_MEM, 2));
  STEP(vif_chain(MX_CH_VIF0, VU0_MEM, 10));
  STEP(vif_chain(MX_CH_VIF1, VU1_MEM, 10));
  STEP(engine(MX_CH_VIF1, VU1_MEM));
  STEP(engine(MX_CH_VIF0, VU0_MEM));
  STEP(mfifo());
  STEP(stall());
  STEP(timer());
  return t_done("dma");
}
