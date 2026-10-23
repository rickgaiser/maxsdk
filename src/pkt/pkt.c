#include <mx/dma.h>
#include <mx/pkt.h>

void mx_pkt_begin(struct mx_pkt *p, void *buf, u32 bytes, u32 flags) {
  p->base = p->cur = buf;
  p->end = (u32 *)((u8 *)buf + (bytes & ~15u));
  p->tag = 0;
  p->flags = flags;
  p->overflow = 0;
}

void mx_pkt_align(struct mx_pkt *p, u32 pos) {
  while (((u32)(p->cur - p->base) & 3) != pos && !p->overflow)
    mx_pkt_w32(p, 0);
}

/* Close the open tag: pad its data to a quadword and fill in QWC. */
static void close_tag(struct mx_pkt *p) {
  if (!p->tag)
    return;
  mx_pkt_align(p, 0);
  u32 qwc = (u32)(p->cur - p->tag) / 4 - 1;
  p->tag[0] = (p->tag[0] & ~0xFFFFu) | qwc;
  p->tag = 0;
}

static void tag(struct mx_pkt *p, u32 id, u32 qwc, const void *addr, u32 c0, u32 c1, int open) {
  if (p->flags & MX_PKT_RAW)
    return;
  close_tag(p);
  mx_pkt_align(p, 0);
  u32 *t = p->cur;
  u64 lo = mx_dmatag(id, qwc, addr, 0);
  int vif = (p->flags & MX_PKT_VIF) != 0;
  mx_pkt_w64(p, lo);
  mx_pkt_w32(p, vif ? c0 : 0);
  mx_pkt_w32(p, vif ? c1 : 0);
  if (open && !p->overflow)
    p->tag = t;
}

void mx_pkt_cnt(struct mx_pkt *p, u32 c0, u32 c1) { tag(p, MX_TAG_CNT, 0, 0, c0, c1, 1); }
void mx_pkt_end(struct mx_pkt *p, u32 c0, u32 c1) { tag(p, MX_TAG_END, 0, 0, c0, c1, 1); }
void mx_pkt_ret(struct mx_pkt *p, u32 c0, u32 c1) { tag(p, MX_TAG_RET, 0, 0, c0, c1, 1); }

void mx_pkt_ref(struct mx_pkt *p, const void *data, u32 qwc, u32 c0, u32 c1) {
  tag(p, MX_TAG_REF, qwc, data, c0, c1, 0);
}
void mx_pkt_refs(struct mx_pkt *p, const void *data, u32 qwc, u32 c0, u32 c1) {
  tag(p, MX_TAG_REFS, qwc, data, c0, c1, 0);
}
void mx_pkt_call(struct mx_pkt *p, const void *chain, u32 c0, u32 c1) {
  tag(p, MX_TAG_CALL, 0, chain, c0, c1, 1);
}
void mx_pkt_next(struct mx_pkt *p, const void *next, u32 c0, u32 c1) {
  tag(p, MX_TAG_NEXT, 0, next, c0, c1, 1);
}

void mx_pkt_irq(struct mx_pkt *p) {
  if (p->tag)
    p->tag[0] |= 0x80000000u;
}

int mx_pkt_finish(struct mx_pkt *p) {
  close_tag(p);
  mx_pkt_align(p, 0);
  return p->overflow ? -1 : (int)mx_pkt_qwc(p);
}

void mx_pkt_writeback(const struct mx_pkt *p) {
  mx_dcache_writeback(p->base, (u32)((u8 *)p->cur - (u8 *)p->base));
}

void mx_pkt_qw(struct mx_pkt *p, u64 lo, u64 hi) {
  mx_pkt_align(p, 0);
  mx_pkt_w64(p, lo);
  mx_pkt_w64(p, hi);
}

void mx_pkt_data(struct mx_pkt *p, const void *src, u32 qwc) {
  mx_pkt_align(p, 0);
  const u32 *s = src;
  for (u32 i = 0; i < qwc * 4; i++)
    mx_pkt_w32(p, s[i]);
}

void mx_pkt_unpack(struct mx_pkt *p, u32 fmt, u32 addr, u32 num, u32 flags, const void *src) {
  mx_pkt_vif(p, mx_vif_unpack(fmt, addr, num, flags));
  u32 words = mx_unpack_words(fmt, num ? num : 256);
  const u32 *s = src;
  for (u32 i = 0; i < words; i++)
    mx_pkt_w32(p, s ? s[i] : 0);
}

void mx_pkt_mpg(struct mx_pkt *p, const void *code, u32 bytes, u32 addr) {
  const u32 *c = code;
  u32 n = bytes / 8;
  while (n) {
    u32 k = n > 256 ? 256 : n;
    /* the code is followed by 64-bit instructions: put it in an odd word */
    if (((u32)(p->cur - p->base) & 1) == 0)
      mx_pkt_w32(p, 0);
    mx_pkt_vif(p, mx_vif_mpg(k, addr));
    for (u32 i = 0; i < k * 2; i++)
      mx_pkt_w32(p, *c++);
    addr += k;
    n -= k;
  }
}

void mx_pkt_direct(struct mx_pkt *p, u32 qwc) {
  mx_pkt_align(p, 3);
  mx_pkt_vif(p, mx_vif_direct(qwc));
}

void mx_pkt_giftag(struct mx_pkt *p, u32 nloop, int eop, s32 prim, u32 flg, u32 nreg, u64 regs) {
  mx_pkt_qw(p, mx_giftag(nloop, eop, prim, flg, nreg), regs);
}
