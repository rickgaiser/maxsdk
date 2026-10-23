#ifndef MX_DMA_H
#define MX_DMA_H

/* DMAC: channels in every mode, and the DMA engine that keeps channels
   running from interrupt-refilled queues.

   Addresses passed here are ordinary pointers (cached, uncached, UCAB or
   scratchpad); they are converted with mx_phys(). Data in cached memory must
   be written back first (mx_dcache_writeback), or built through an uncached
   pointer. */

#include <mx/bits.h>
#include <mx/cpu.h>
#include <mx/hw/formats.h>
#include <mx/hw/regs.h>
#include <mx/types.h>

/* Channel numbers (also the D_STAT/D_PCR bit numbers). */
enum {
  MX_CH_VIF0 = MX_DMAC_VIF0,
  MX_CH_VIF1 = MX_DMAC_VIF1,
  MX_CH_GIF = MX_DMAC_GIF,
  MX_CH_FROMIPU = MX_DMAC_FROMIPU,
  MX_CH_TOIPU = MX_DMAC_TOIPU,
  MX_CH_FROMSPR = MX_DMAC_FROMSPR,
  MX_CH_TOSPR = MX_DMAC_TOSPR,
};

extern const u32 mx_dma_base[10];

#define MX_DMA_REG(ch, reg) MX_R32(mx_dma_base[ch] + MX_DMA_##reg##_OFFSET)

/* CHCR values */
#define MX_CHCR_TO_MEM 0 /* DIR */
#define MX_CHCR_FROM_MEM 1
#define MX_CHCR_NORMAL MX_SET(DMA_CHCR_MOD, 0)
#define MX_CHCR_CHAIN MX_SET(DMA_CHCR_MOD, 1)
#define MX_CHCR_INTERLEAVE MX_SET(DMA_CHCR_MOD, 2)
#define MX_CHCR_TTE MX_BIT(DMA_CHCR_TTE)
#define MX_CHCR_TIE MX_BIT(DMA_CHCR_TIE)
#define MX_CHCR_STR MX_BIT(DMA_CHCR_STR)

/* ---- DMA tags (source chain) ------------------------------------------- */

MX_INLINE u64 mx_dmatag(u32 id, u32 qwc, const void *addr, int irq) {
  return MX_SET(DMATAG_QWC, qwc) | MX_SET(DMATAG_ID, id) | MX_SET(DMATAG_IRQ, irq) |
         ((u64)(addr ? mx_phys(addr) : 0) << 32);
}

/* ---- Channels ---------------------------------------------------------- */

/* Enable the DMAC (D_CTRL.DMAE) and clear stale channel status. */
void mx_dma_init(void);

MX_INLINE int mx_dma_busy(int ch) { return (MX_DMA_REG(ch, CHCR) & MX_CHCR_STR) != 0; }
/* Poll until channel ch has stopped. Returns 0, or -1 after ~1 s. */
int mx_dma_wait(int ch);

/* Normal mode: qwc quadwords from addr to the peripheral (VIF0/VIF1/GIF/
   toIPU) or from the peripheral to addr (fromIPU). */
void mx_dma_normal(int ch, const void *addr, u32 qwc);
/* Source chain mode starting at the tag at tags. With tte, the upper 64 bits
   of each tag (VIF codes) are sent too (VIF0/VIF1). */
void mx_dma_chain(int ch, const void *tags, int tte);
/* Scratchpad <-> memory, normal mode. */
void mx_dma_to_spr(void *spr, const void *mem, u32 qwc);
void mx_dma_from_spr(void *mem, const void *spr, u32 qwc);
/* Interleave: move tqwc quadwords, skip sqwc, until qwc are moved. */
void mx_dma_to_spr_interleave(void *spr, const void *mem, u32 qwc, u32 tqwc, u32 sqwc);
void mx_dma_from_spr_interleave(void *mem, const void *spr, u32 qwc, u32 tqwc, u32 sqwc);
/* Destination chain on fromSPR: the stream at spr holds tags (cnt/cnts/end)
   whose ADDR is the destination in memory, each followed by its data. */
void mx_dma_from_spr_chain(const void *spr);

/* Stall control: the drain (MX_STALL_DRAIN_*) never passes the source
   (MX_STALL_SRC_*). 0, 0 turns it off. */
enum { MX_STALL_SRC_SIF0 = 1, MX_STALL_SRC_FROMSPR = 2, MX_STALL_SRC_FROMIPU = 3 };
enum { MX_STALL_DRAIN_VIF1 = 1, MX_STALL_DRAIN_SIF1 = 2, MX_STALL_DRAIN_GIF = 3 };
void mx_dma_stall(int src, int drain, const void *start);

/* ---- MFIFO ------------------------------------------------------------- */

/* Ring buffer in main memory between fromSPR (the source) and VIF1 or GIF
   (the drain). size is a power of two in bytes, ring is aligned to it. The
   drain is started and then holds while the ring is empty. tte: the drain
   (VIF1) also sends the VIF codes in the upper half of each tag. */
void mx_mfifo_init(int drain_ch, void *ring, u32 size, int tte);
/* Copy qwc quadwords from the scratchpad into the ring (fromSPR). They must
   be whole drain packets: cnt/ref/end tags with their data. Waits for space,
   and returns while the copy runs: the SPR source must not be changed before
   mx_mfifo_push_wait() (or the next push) - double-buffer it. */
void mx_mfifo_push(const void *spr, u32 qwc);
MX_INLINE void mx_mfifo_push_wait(void) { mx_dma_wait(MX_CH_FROMSPR); }
/* Quadwords in the ring not yet read by the drain. */
u32 mx_mfifo_used(void);
/* Stop the MFIFO (waits until the drain has emptied the ring). */
void mx_mfifo_stop(void);

/* ---- The DMA engine: interrupt-refilled queues --------------------------- */

#define MX_DMAQ_LEN 64

struct mx_dma_seg {
  u32 chcr; /* mode bits (CHAIN/NORMAL, TTE, DIR) without STR */
  u32 addr; /* TADR (chain) or MADR (normal), physical */
  u32 qwc;  /* normal mode */
};

/* An event: the queue's done counter passing seq. */
typedef struct {
  struct mx_dmaq *q;
  u32 seq;
} mx_event;

struct mx_dmaq {
  int ch;
  volatile u32 head, tail; /* ring of queued segments */
  volatile u32 done;       /* segments completed (the fence) */
  u32 issued;              /* segments submitted */
  volatile int running;
  struct mx_dma_seg seg[MX_DMAQ_LEN];
  u32 t_last_end; /* Count at the last end interrupt */
  /* a thread sleeping in mx_event_sleep (<mx/thread.h>) */
  volatile int sleep_sema; /* semaphore to signal, -1: nobody sleeps */
  volatile u32 sleep_seq;
  int sema;                /* the queue's semaphore, -1 until first needed */
};

/* Take over channel ch: its end interrupt starts the next queued segment. */
void mx_dmaq_init(struct mx_dmaq *q, int ch);
void mx_dmaq_release(struct mx_dmaq *q);
/* Queue a source chain (tags must be complete) / a normal transfer. Returns
   at once; the event passes when that segment has been transferred. */
mx_event mx_dmaq_chain(struct mx_dmaq *q, const void *tags, int tte);
mx_event mx_dmaq_normal(struct mx_dmaq *q, const void *addr, u32 qwc);

MX_INLINE int mx_event_done(mx_event e) { return (s32)(e.q->done - e.seq) >= 0; }
void mx_event_wait(mx_event e);
/* Wait until everything queued so far is done. */
void mx_dmaq_drain(struct mx_dmaq *q);

#endif
