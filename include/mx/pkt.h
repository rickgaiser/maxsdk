#ifndef MX_PKT_H
#define MX_PKT_H

/* Packet builder: DMA source chains with VIF or GIF data (decision D5b).
   It tracks the open DMA tag, pads to quadwords, closes tags itself and
   stops (sets overflow) instead of writing past the buffer.

   A packet is built in any memory the EE can write: cached (then
   mx_pkt_writeback() before sending), uncached, UCAB or the scratchpad. */

#include <mx/cpu.h>
#include <mx/gif.h>
#include <mx/types.h>
#include <mx/vif.h>

/* flags */
#define MX_PKT_VIF 1 /* for VIF0/VIF1 with TTE: tags carry two VIF codes */
#define MX_PKT_RAW 2 /* no DMA tags at all (for normal-mode transfers) */

struct mx_pkt {
  u32 *base, *cur, *end;
  u32 *tag; /* open cnt/end tag, or 0 */
  u32 flags;
  int overflow;
};

void mx_pkt_begin(struct mx_pkt *p, void *buf, u32 bytes, u32 flags);
/* Close the open tag and return the size in quadwords (-1 on overflow). */
int mx_pkt_finish(struct mx_pkt *p);
MX_INLINE u32 mx_pkt_qwc(const struct mx_pkt *p) { return (u32)(p->cur - p->base + 3) / 4; }
MX_INLINE void *mx_pkt_start(const struct mx_pkt *p) { return p->base; }
void mx_pkt_writeback(const struct mx_pkt *p);

/* ---- DMA tags ---------------------------------------------------------- */

/* Open a cnt packet: the data that follows is sent. c0, c1: VIF codes in the
   tag (MX_PKT_VIF), otherwise ignored. */
void mx_pkt_cnt(struct mx_pkt *p, u32 c0, u32 c1);
/* ref (or refs: stall controlled) packet of qwc quadwords at data. */
void mx_pkt_ref(struct mx_pkt *p, const void *data, u32 qwc, u32 c0, u32 c1);
void mx_pkt_refs(struct mx_pkt *p, const void *data, u32 qwc, u32 c0, u32 c1);
/* call a sub-chain (which ends with mx_pkt_ret), ret, next */
void mx_pkt_call(struct mx_pkt *p, const void *chain, u32 c0, u32 c1);
void mx_pkt_ret(struct mx_pkt *p, u32 c0, u32 c1);
void mx_pkt_next(struct mx_pkt *p, const void *next, u32 c0, u32 c1);
/* Open an end packet: the data that follows is the last of the chain. */
void mx_pkt_end(struct mx_pkt *p, u32 c0, u32 c1);
/* The IRQ bit on the open tag: the channel stops after its packet and
   raises its interrupt (with CHCR.TIE). */
void mx_pkt_irq(struct mx_pkt *p);

/* ---- data ------------------------------------------------------------- */

MX_INLINE void mx_pkt_w32(struct mx_pkt *p, u32 v) {
  if (p->cur < p->end)
    *p->cur++ = v;
  else
    p->overflow = 1;
}
MX_INLINE void mx_pkt_w64(struct mx_pkt *p, u64 v) {
  mx_pkt_w32(p, (u32)v);
  mx_pkt_w32(p, (u32)(v >> 32));
}
/* Pad with zeros (VIF NOPs) until the word position within the quadword
   is pos (0..3). */
void mx_pkt_align(struct mx_pkt *p, u32 pos);
/* A quadword (aligned first). */
void mx_pkt_qw(struct mx_pkt *p, u64 lo, u64 hi);
/* Copy qwc quadwords (aligned first). */
void mx_pkt_data(struct mx_pkt *p, const void *src, u32 qwc);

/* ---- VIF -------------------------------------------------------------- */

MX_INLINE void mx_pkt_vif(struct mx_pkt *p, u32 code) { mx_pkt_w32(p, code); }
/* UNPACK of num vectors with the data copied from src. */
void mx_pkt_unpack(struct mx_pkt *p, u32 fmt, u32 addr, u32 num, u32 flags, const void *src);
/* MPG: upload bytes of microcode (8-byte instructions) to micro memory addr
   (64-bit units), split into codes of up to 256 instructions. */
void mx_pkt_mpg(struct mx_pkt *p, const void *code, u32 bytes, u32 addr);
/* DIRECT of qwc quadwords: the code is placed so the GIF data that follows
   starts on a quadword. */
void mx_pkt_direct(struct mx_pkt *p, u32 qwc);

/* ---- GIF -------------------------------------------------------------- */

void mx_pkt_giftag(struct mx_pkt *p, u32 nloop, int eop, s32 prim, u32 flg, u32 nreg, u64 regs);
/* A+D: a GIF tag for n register writes, then mx_pkt_ad() n times. */
MX_INLINE void mx_pkt_ad_begin(struct mx_pkt *p, u32 n, int eop) {
  mx_pkt_giftag(p, n, eop, -1, MX_GIF_FLG_PACKED, 1, MX_GIF_REG_AD);
}
MX_INLINE void mx_pkt_ad(struct mx_pkt *p, u32 reg, u64 v) {
  mx_pkt_w64(p, v);
  mx_pkt_w64(p, reg);
}

#endif
