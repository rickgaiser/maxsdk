#include <mx/bios.h>
#include <mx/debug.h>
#include <mx/dma.h>
#include <mx/irq.h>
#include <mx/timer.h>
#include <mx/trace.h>
#include <mx/vu.h>

const u32 mx_dma_base[10] = {
    MX_DMA_VIF0,    MX_DMA_VIF1, MX_DMA_GIF,  MX_DMA_FROMIPU, MX_DMA_TOIPU,
    MX_DMA_SIF0,    MX_DMA_SIF1, MX_DMA_SIF2, MX_DMA_FROMSPR, MX_DMA_TOSPR,
};

static void start(int ch, u32 chcr) {
  mx_sync();
  mx_trace(MX_TR_DMA_START, (u16)ch);
  MX_DMA_REG(ch, CHCR) = chcr | MX_CHCR_STR;
}

void mx_dma_init(void) {
  MX_R32(MX_D_CTRL) |= MX_BIT(D_CTRL_DMAE);
  /* clear the end status of our channels (not SIF, the BIOS owns those) */
  MX_R32(MX_D_STAT) = 0x31F & MX_D_STAT_CIS_MASK;
}

int mx_dma_wait(int ch) {
  u32 t0 = mx_count();
  while (mx_dma_busy(ch))
    if (mx_count() - t0 > 300000000u)
      return -1;
  return 0;
}

void mx_dma_normal(int ch, const void *addr, u32 qwc) {
  MX_DMA_REG(ch, MADR) = mx_phys(addr);
  MX_DMA_REG(ch, QWC) = qwc;
  start(ch, MX_CHCR_NORMAL | (ch == MX_CH_FROMIPU ? MX_CHCR_TO_MEM : MX_CHCR_FROM_MEM));
}

void mx_dma_chain(int ch, const void *tags, int tte) {
  MX_DMA_REG(ch, QWC) = 0;
  MX_DMA_REG(ch, TADR) = mx_phys(tags);
  start(ch, MX_CHCR_CHAIN | MX_CHCR_FROM_MEM | (tte ? MX_CHCR_TTE : 0));
}

void mx_dma_to_spr(void *spr, const void *mem, u32 qwc) {
  MX_DMA_REG(MX_CH_TOSPR, MADR) = mx_phys(mem);
  MX_DMA_REG(MX_CH_TOSPR, SADR) = (u32)(uintptr_t)spr & 0x3FF0u;
  MX_DMA_REG(MX_CH_TOSPR, QWC) = qwc;
  start(MX_CH_TOSPR, MX_CHCR_NORMAL);
}

void mx_dma_from_spr(void *mem, const void *spr, u32 qwc) {
  MX_DMA_REG(MX_CH_FROMSPR, MADR) = mx_phys(mem);
  MX_DMA_REG(MX_CH_FROMSPR, SADR) = (u32)(uintptr_t)spr & 0x3FF0u;
  MX_DMA_REG(MX_CH_FROMSPR, QWC) = qwc;
  start(MX_CH_FROMSPR, MX_CHCR_NORMAL);
}

static void set_sqwc(u32 tqwc, u32 sqwc) {
  MX_R32(MX_D_SQWC) = (u32)(MX_SET(D_SQWC_SQWC, sqwc) | MX_SET(D_SQWC_TQWC, tqwc));
}

void mx_dma_to_spr_interleave(void *spr, const void *mem, u32 qwc, u32 tqwc, u32 sqwc) {
  set_sqwc(tqwc, sqwc);
  MX_DMA_REG(MX_CH_TOSPR, MADR) = mx_phys(mem);
  MX_DMA_REG(MX_CH_TOSPR, SADR) = (u32)(uintptr_t)spr & 0x3FF0u;
  MX_DMA_REG(MX_CH_TOSPR, QWC) = qwc;
  start(MX_CH_TOSPR, MX_CHCR_INTERLEAVE);
}

void mx_dma_from_spr_interleave(void *mem, const void *spr, u32 qwc, u32 tqwc, u32 sqwc) {
  set_sqwc(tqwc, sqwc);
  MX_DMA_REG(MX_CH_FROMSPR, MADR) = mx_phys(mem);
  MX_DMA_REG(MX_CH_FROMSPR, SADR) = (u32)(uintptr_t)spr & 0x3FF0u;
  MX_DMA_REG(MX_CH_FROMSPR, QWC) = qwc;
  start(MX_CH_FROMSPR, MX_CHCR_INTERLEAVE);
}

void mx_dma_from_spr_chain(const void *spr) {
  MX_DMA_REG(MX_CH_FROMSPR, SADR) = (u32)(uintptr_t)spr & 0x3FF0u;
  MX_DMA_REG(MX_CH_FROMSPR, QWC) = 0;
  start(MX_CH_FROMSPR, MX_CHCR_CHAIN);
}

void mx_dma_stall(int src, int drain, const void *startp) {
  u32 c = MX_R32(MX_D_CTRL);
  c &= ~(u32)(MX_BIT(D_CTRL_STS) | MX_BIT(D_CTRL_STD));
  c |= (u32)(MX_SET(D_CTRL_STS, src) | MX_SET(D_CTRL_STD, drain));
  if (startp)
    MX_R32(MX_D_STADR) = mx_phys(startp);
  MX_R32(MX_D_CTRL) = c;
}

/* ---- MFIFO ------------------------------------------------------------- */

static struct {
  int drain;
  u32 ring, size; /* physical start, bytes */
} mf;

void mx_mfifo_init(int drain_ch, void *ring, u32 size, int tte) {
  mf.drain = drain_ch;
  mf.ring = mx_phys(ring);
  mf.size = size;
  MX_R32(MX_D_RBOR) = mf.ring;
  MX_R32(MX_D_RBSR) = size - 16;
  MX_DMA_REG(MX_CH_FROMSPR, MADR) = mf.ring;
  u32 c = MX_R32(MX_D_CTRL) & ~(u32)MX_BIT(D_CTRL_MFD);
  MX_R32(MX_D_CTRL) = c | (u32)MX_SET(D_CTRL_MFD, drain_ch == MX_CH_VIF1 ? 2 : 3);
  MX_DMA_REG(drain_ch, QWC) = 0;
  MX_DMA_REG(drain_ch, TADR) = mf.ring;
  start(drain_ch, MX_CHCR_CHAIN | MX_CHCR_FROM_MEM | (tte ? MX_CHCR_TTE : 0));
}

u32 mx_mfifo_used(void) {
  u32 w = MX_DMA_REG(MX_CH_FROMSPR, MADR);
  u32 r = MX_DMA_REG(mf.drain, TADR);
  return ((w - r) & (mf.size - 1)) / 16;
}

/* The ring has not drained for a second: report where the drain stands. */
static void mfifo_stalled(void) {
  mx_printf("mx_mfifo_push: the ring has not drained for 1 s: %s CHCR %08x MADR %08x "
            "TADR %08x QWC %u; VIF1_STAT %08x VIF1_CODE %08x GIF_STAT %08x VPU_STAT %08x\n",
            mf.drain == MX_CH_VIF1 ? "VIF1" : "GIF", MX_DMA_REG(mf.drain, CHCR),
            MX_DMA_REG(mf.drain, MADR), MX_DMA_REG(mf.drain, TADR), MX_DMA_REG(mf.drain, QWC),
            MX_R32(MX_VIF_VIF1_STAT), MX_R32(MX_VIF_VIF1_CODE), MX_R32(MX_GIF_STAT),
            mx_vpu_stat());
}

void mx_mfifo_push(const void *spr, u32 qwc) {
  mx_dma_wait(MX_CH_FROMSPR);
  /* keep one quadword free, so full and empty differ */
  u32 t0 = mx_count();
  int reported = 0;
  while (mx_mfifo_used() + qwc >= mf.size / 16)
    if (!reported && mx_count() - t0 > MX_EE_HZ) {
      mfifo_stalled();
      reported = 1;
    }
  MX_DMA_REG(MX_CH_FROMSPR, SADR) = (u32)(uintptr_t)spr & 0x3FF0u;
  MX_DMA_REG(MX_CH_FROMSPR, QWC) = qwc;
  start(MX_CH_FROMSPR, MX_CHCR_NORMAL);
}

void mx_mfifo_stop(void) {
  mx_dma_wait(MX_CH_FROMSPR);
  while (mx_mfifo_used())
    ;
  /* the drain holds on the empty ring: stop it, then leave MFIFO mode */
  MX_DMA_REG(mf.drain, CHCR) &= ~(u32)MX_CHCR_STR;
  MX_R32(MX_D_CTRL) &= ~(u32)MX_BIT(D_CTRL_MFD);
}

/* ---- The DMA engine ------------------------------------------------------ */

static void q_start(struct mx_dmaq *q, struct mx_dma_seg *s) {
  int ch = q->ch;
  if (s->chcr & MX_CHCR_CHAIN) {
    MX_DMA_REG(ch, QWC) = 0;
    MX_DMA_REG(ch, TADR) = s->addr;
  } else {
    MX_DMA_REG(ch, MADR) = s->addr;
    MX_DMA_REG(ch, QWC) = s->qwc;
  }
  start(ch, s->chcr);
}

/* Channel end: the segment at head is done; start the next one at once. */
static void q_irq(void *arg) {
  struct mx_dmaq *q = arg;
  q->t_last_end = mx_count();
  if (!q->running || mx_dma_busy(q->ch))
    return; /* not ours (a transfer started outside the queue) */
  u32 h = q->head + 1;
  q->head = h;
  q->done++;
  if (q->sleep_sema >= 0 && (s32)(q->done - q->sleep_seq) >= 0) {
    mx_bios_isignal_sema(q->sleep_sema);
    q->sleep_sema = -1;
  }
  if (h != q->tail)
    q_start(q, &q->seg[h % MX_DMAQ_LEN]);
  else
    q->running = 0;
  mx_trace(MX_TR_DMA_END, (u16)q->ch);
}

void mx_dmaq_init(struct mx_dmaq *q, int ch) {
  q->ch = ch;
  q->head = q->tail = 0;
  q->done = q->issued = 0;
  q->running = 0;
  q->sleep_sema = -1;
  q->sema = -1;
  /* a stale end status would fire as soon as the interrupt is enabled */
  MX_R32(MX_D_STAT) = 1u << ch;
  mx_irq_dmac(ch, q_irq, q);
}

void mx_dmaq_release(struct mx_dmaq *q) {
  mx_dmaq_drain(q);
  mx_irq_dmac(q->ch, 0, 0);
}

static mx_event q_submit(struct mx_dmaq *q, u32 chcr, u32 addr, u32 qwc) {
  /* wait for a free slot */
  while (q->tail - q->head >= MX_DMAQ_LEN)
    ;
  int was = mx_di();
  u32 t = q->tail;
  struct mx_dma_seg *s = &q->seg[t % MX_DMAQ_LEN];
  s->chcr = chcr;
  s->addr = addr;
  s->qwc = qwc;
  q->tail = t + 1;
  mx_event e = {q, ++q->issued};
  if (!q->running) {
    q->running = 1;
    q_start(q, s);
  }
  mx_irq_restore(was);
  return e;
}

mx_event mx_dmaq_chain(struct mx_dmaq *q, const void *tags, int tte) {
  return q_submit(q, MX_CHCR_CHAIN | MX_CHCR_FROM_MEM | (tte ? MX_CHCR_TTE : 0), mx_phys(tags),
                  0);
}

mx_event mx_dmaq_normal(struct mx_dmaq *q, const void *addr, u32 qwc) {
  return q_submit(q, MX_CHCR_NORMAL | MX_CHCR_FROM_MEM, mx_phys(addr), qwc);
}

void mx_event_wait(mx_event e) {
  mx_trace(MX_TR_WAIT_BEGIN, (u16)e.q->ch);
  while (!mx_event_done(e))
    ;
  mx_trace(MX_TR_WAIT_END, (u16)e.q->ch);
}

void mx_dmaq_drain(struct mx_dmaq *q) {
  mx_event e = {q, q->issued};
  mx_event_wait(e);
}
