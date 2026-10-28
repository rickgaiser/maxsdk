/* Chains built once and sent many times (<mx/chain.h>). */

#include <mx/chain.h>

int mx_chain_close(struct mx_chain *ch, struct mx_pbuf *b, u32 id) {
  u64 *c = mx_pbuf_reserve(b, 1);
  if (!c || b->overflow) {
    ch->qwc = 0;
    return -1;
  }
  c = mx_e_tag(c, id, 0, 0, 0, 0);
  mx_pbuf_commit(b, c);
  ch->qwc = (u32)(c - ch->start) / 2;
  mx_dcache_writeback(ch->start, ch->qwc * 16);
  return 0;
}
