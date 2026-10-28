#ifndef MX_CHAIN_H
#define MX_CHAIN_H

/* Chains: DMA source chains built once in memory and sent any number of
   times: on their own, through a DMA queue, or called (CALL tag) from
   another chain such as the frame stream (mx_frame_call). A model's batches
   or a texture's IMAGE packets are built as a chain when the asset is
   loaded; a frame then references them, one CALL each.

   A chain is built with the cursor emitters (<mx/emit.h>) into a packet
   buffer (mx_pbuf) and closed with a ret tag (callable; sent on its own the
   ret ends the transfer, as nothing was called) or an end tag. Calls nest
   two deep (Dn_ASR0/1): a chain called from the frame stream may call one
   more; a third CALL stops the channel (D_STAT.CIS).

   MFIFO drains only take cnt, ref, refs, refe and end tags (EE User's
   Manual 5.5): the frame stream calls chains in chain mode only
   (mx_frame_init_chain). */

#include <mx/dma.h>
#include <mx/emit.h>
#include <mx/types.h>

/* ---- packet buffers --------------------------------------------------------- */

/* Memory that chains are built in: cached (written back when a chain is
   closed), uncached, or the scratchpad (sent from there). */
struct mx_pbuf {
  u64 *base, *cur, *end;
  int overflow; /* a reserve did not fit */
};

/* mem: quadword aligned. */
MX_INLINE void mx_pbuf_init(struct mx_pbuf *b, void *mem, u32 bytes) {
  b->base = b->cur = (u64 *)mem;
  b->end = b->base + bytes / 16 * 2;
  b->overflow = 0;
}
/* Room for qwc quadwords: the cursor, or 0 (overflow set) if they do not
   fit. */
MX_INLINE u64 *mx_pbuf_reserve(struct mx_pbuf *b, u32 qwc) {
  if (b->cur + 2 * qwc > b->end) {
    b->overflow = 1;
    return 0;
  }
  return b->cur;
}
MX_INLINE void mx_pbuf_commit(struct mx_pbuf *b, u64 *end) { b->cur = end; }
MX_INLINE u32 mx_pbuf_used_qw(const struct mx_pbuf *b) { return (u32)(b->cur - b->base) / 2; }
/* Forget every chain built in b (they must no longer be sent). */
MX_INLINE void mx_pbuf_reset(struct mx_pbuf *b) {
  b->cur = b->base;
  b->overflow = 0;
}

/* ---- chains ----------------------------------------------------------------- */

struct mx_chain {
  const u64 *start;
  u32 qwc; /* quadwords of the chain itself, tags included (0: none) */
};

/* The chain starts at the buffer's cursor. */
MX_INLINE void mx_chain_begin(struct mx_chain *ch, const struct mx_pbuf *b) {
  ch->start = b->cur;
  ch->qwc = 0;
}
/* Close the chain with a ret (MX_TAG_RET) or end (MX_TAG_END) tag of no data
   (VIF NOPs in its upper half) and write it back from the data cache.
   Returns 0, or -1 if the buffer overflowed (the chain is not usable). */
int mx_chain_close(struct mx_chain *ch, struct mx_pbuf *b, u32 id);
/* Built and closed, with more than its closing tag. */
MX_INLINE int mx_chain_empty(const struct mx_chain *ch) { return ch->qwc <= 1; }

/* A CALL of ch in another chain (the tag's VIF codes: NOPs). */
MX_INLINE u64 *mx_e_call_chain(u64 *c, const struct mx_chain *ch) {
  return mx_e_call(c, ch->start, 0, 0, 0);
}

/* Send ch on DMA channel dma_ch now (the channel must be idle); tte for VIF0
   and VIF1 (the VIF codes in the tags). */
MX_INLINE void mx_chain_send(int dma_ch, const struct mx_chain *ch, int tte) {
  mx_dma_chain(dma_ch, ch->start, tte);
}
/* Queue ch (the event passes when it has been sent). */
MX_INLINE mx_event mx_chain_queue(struct mx_dmaq *q, const struct mx_chain *ch, int tte) {
  return mx_dmaq_chain(q, ch->start, tte);
}

#endif
