/* The PATH3 stream (<mx/frame.h>, mx_frame3_*): GIF packets built in two
   scratchpad halves, copied into slots in memory, sent by a queue on the
   GIF channel. Frames are kept apart from the VIF1 stream by two I-bit
   MARKs in it:
     open n  (MARK 0x8000 | n, an I-bit NOP): where frame n's first PATH3 packet was
             reserved. The VIF1 interrupt queues frame n's slots that were
             held until then, and lets VIF1 go on.
     join n  (MARK 0xC000 | n, an I-bit NOP), after FLUSH, before FLUSHA and the FINISH:
             VIF1 goes on once the GIF queue has sent all of frame n's
             slots (the VIF1 interrupt, or the GIF queue's end interrupt).
   n is the frame number's low 14 bits; bit 15 keeps the marks apart from
   mx_frame_present's (gs.c). */

#include <mx/debug.h>
#include <mx/frame.h>
#include <mx/irq.h>
#include <mx/mx.h>
#include <mx/timer.h>
#include <mx/vif.h>
#include "frame_int.h"

struct mx_frame3_stats mx_frame3_stats;

#define NREC 8

static struct {
  u64 *half[2];
  u64 *pos, *end, *tag; /* tag: the cnt tag of the packets being reserved */
  u32 half_qw;
  int cur;
  u64 *slot[MX_FRAME_SLOTS];
  mx_event ev[MX_FRAME_SLOTS];
  u8 used[MX_FRAME_SLOTS], queued[MX_FRAME_SLOTS];
  u32 nslot, next;
  struct mx_dmaq q;
  int open;  /* a frame's packets are being built */
  u32 frame; /* its number */
  /* slots of frames whose open mark VIF1 has not passed */
  u32 hold_slot[MX_FRAME_SLOTS], hold_frame[MX_FRAME_SLOTS];
  volatile u32 hold_head, hold_tail;
  /* the frames in flight, by n & 7 */
  struct {
    volatile u32 n;                       /* low 14 bits of the frame number */
    volatile u32 last_seq;                /* the queue's event of its last slot */
    volatile u8 opened, complete, joined; /* joined: VIF1 let past the join */
  } rec[NREC];
} p;

static u32 n14(u32 frame) { return frame & 0x3FFF; }

/* The stop after a mark: an I-bit NOP (PCSX2 does not stop at an I-bit
   MARK) */
#define STOP (mx_vif_nop() | MX_VIF_IBIT)

/* Queue slot k (interrupts off). */
static void queue_slot(u32 k, u32 rn) {
  mx_dmaq_try_chain(&p.q, p.slot[k], 0, &p.ev[k]);
  p.queued[k] = 1;
  p.rec[rn & (NREC - 1)].last_seq = p.ev[k].seq;
}

/* VIF1 stopped at one of our marks: let it go on when its condition holds.
   From the VIF1 and the GIF queue's interrupts. The mark is read from
   VIF1_MARK (PCSX2's VIF1_CODE does not show it), which keeps it after VIF1
   has gone on: each mark is acted on once (opened), VIF1 let go on each
   time the condition holds. */
static void release(void) {
  /* INT: the code with the I-bit is done; VIS follows once VIF1 tries the
     next code. */
  if (!(MX_R32(MX_VIF_VIF1_STAT) & (MX_BIT(VIF_STAT_VIS) | MX_BIT(VIF_STAT_INT))))
    return;
  u32 mark = MX_R32(MX_VIF_VIF1_MARK);
  if (!(mark & 0x8000))
    return;
  u32 n = mark & 0x3FFF;
  __typeof__(p.rec[0]) *r = &p.rec[n & (NREC - 1)];
  if (r->n != n)
    return;
  if (mark & 0x4000) {
    /* join: all of the frame's slots sent */
    if (!r->complete || (s32)(p.q.done - r->last_seq) < 0)
      return;
    r->joined = 1;
  } else if (!r->opened) {
    /* open: the slots held for it go to the queue */
    r->opened = 1;
    while (p.hold_head != p.hold_tail && p.hold_frame[p.hold_head % MX_FRAME_SLOTS] == n) {
      queue_slot(p.hold_slot[p.hold_head % MX_FRAME_SLOTS], n);
      p.hold_head++;
    }
  }
  /* The PS2 stops VIF1 even after an STC written between INT and VIS (it
     stayed at the join for good): wait for the stop. It comes as VIF1 takes
     the next code, which is in the same chain (the FLUSHA after the join's
     NOP, the end tag's NOPs after the open's); a bound all the same. */
  for (u32 t0 = mx_count();
       !(MX_R32(MX_VIF_VIF1_STAT) & MX_BIT(VIF_STAT_VIS)) && mx_count() - t0 < 100000;)
    ;
  MX_R32(MX_VIF_VIF1_FBRST) = (u32)MX_BIT(VIF_FBRST_STC);
}

static void report(void) {
  mx_printf("mx_frame3: frame %u open %u, queue %u of %u done, held %u-%u\n", n14(p.frame), p.open,
            p.q.done, p.q.issued, p.hold_head, p.hold_tail);
  for (int i = 0; i < NREC; i++)
    if (p.rec[i].n != ~0u)
      mx_printf("mx_frame3: frame %u: opened %u complete %u joined %u, last %u\n", p.rec[i].n,
                p.rec[i].opened, p.rec[i].complete, p.rec[i].joined, p.rec[i].last_seq);
}

static void vif1_irq(void *arg) {
  (void)arg;
  release();
}

static void gif_done(void *arg) {
  (void)arg;
  release();
}

static int slot_busy(u32 k) { return p.used[k] && (!p.queued[k] || !mx_event_done(p.ev[k])); }

/* The current half (with its end tag) into the next slot, which is queued
   or held. */
static void flush(void) {
  u32 n = (u32)(p.pos - p.half[p.cur]) / 2;
  if (!n)
    return;
  p.pos = mx_e_end(p.pos, 0, 0, 0);
  u32 k = p.next;
  p.next = (k + 1) % p.nslot;
  if (slot_busy(k)) {
    /* the slot may be held for an open mark still in the VIF1 stream */
    mx__frame_kick();
    u32 t0 = mx_count();
    int reported = 0;
    while (slot_busy(k))
      if (!reported && mx_count() - t0 > MX_EE_HZ) {
        mx__frame_stalled("a slot of the PATH3 stream not sent");
        reported = 1;
      }
    mx_frame3_stats.wait_slot += mx_count() - t0;
  }
  mx_dma_wait(MX_CH_FROMSPR);
  mx_dma_from_spr(p.slot[k], p.half[p.cur], n + 1);
  mx_dma_wait(MX_CH_FROMSPR);
  p.used[k] = 1;
  int was = mx_di();
  __typeof__(p.rec[0]) *r = &p.rec[n14(p.frame) & (NREC - 1)];
  if (r->opened) {
    queue_slot(k, n14(p.frame));
  } else {
    p.queued[k] = 0;
    p.hold_slot[p.hold_tail % MX_FRAME_SLOTS] = k;
    p.hold_frame[p.hold_tail % MX_FRAME_SLOTS] = n14(p.frame);
    p.hold_tail++;
    mx_frame3_stats.held++;
  }
  mx_irq_restore(was);
  mx_frame3_stats.slots++;
  mx_frame3_stats.qw += n + 1;
  p.cur ^= 1;
  p.pos = p.half[p.cur];
  p.end = p.pos + 2 * (p.half_qw - 1);
}

/* The frame's first packet: its open mark at the VIF1 stream's end. */
static void open_frame(void) {
  if (!mx__frame3_end) {
    mx_printf("mx_frame3: the PATH3 stream is not on (mx_frame3_init)\n");
    mx_exit(254);
  }
  p.frame = mx_gs.frames;
  u32 n = n14(p.frame);
  __typeof__(p.rec[0]) *r = &p.rec[n & (NREC - 1)];
  int was = mx_di();
  r->n = n;
  r->opened = r->complete = r->joined = 0;
  r->last_seq = p.q.issued;
  mx_irq_restore(was);
  p.open = 1;
  mx_frame_commit(mx_e_cnt(mx_frame_reserve(1), 0, mx_vif_mark(0x8000 | n), STOP));
  /* to VIF1 now: the slots start once it gets there */
  mx__frame_kick();
}

static void frame_end(void) {
  if (!p.open)
    return;
  p.open = 0;
  flush();
  u32 n = n14(p.frame);
  p.rec[n & (NREC - 1)].complete = 1;
  u64 *c = mx_frame_reserve(2);
  c = mx_e_cnt(c, 0, mx_vif_flush(), mx_vif_mark(0xC000 | n));
  mx_frame_commit(mx_e_cnt(c, 0, STOP, mx_vif_flusha()));
}

/* A frame whose join VIF1 has not passed yet. */
static int join_ahead(void) {
  for (int i = 0; i < NREC; i++)
    if (p.rec[i].n != ~0u && p.rec[i].complete && !p.rec[i].joined)
      return 1;
  return 0;
}

static void stop(void) {
  /* the VIF1 interrupt lets VIF1 past the joins still in its FIFO */
  u32 t0 = mx_count();
  int reported = 0;
  while (join_ahead())
    if (!reported && mx_count() - t0 > MX_EE_HZ) {
      mx__frame_stalled("mx_frame_stop: a join of the PATH3 stream not passed");
      reported = 1;
    }
  mx_dmaq_release(&p.q);
  mx_irq_intc(MX_INTC_VIF1, 0, 0);
  mx__frame3_end = 0;
  mx__frame3_stop = 0;
  mx__frame3_q = 0;
  mx__frame3_report = 0;
}

int mx_frame3_init(void *mem, u32 bytes, void *spr, u32 spr_bytes) {
  if (!mx__frame_chain())
    return -1;
  u32 half = spr_bytes / 2 & ~15u;
  if (half > 0x2000)
    half = 0x2000;
  p.nslot = bytes / half;
  if (p.nslot > MX_FRAME_SLOTS)
    p.nslot = MX_FRAME_SLOTS;
  if (p.nslot < 2)
    return -1;
  p.half[0] = spr;
  p.half[1] = (u64 *)((u8 *)spr + half);
  p.half_qw = half / 16;
  p.cur = 0;
  p.pos = p.half[0];
  p.end = p.pos + 2 * (p.half_qw - 1);
  mx_dcache_invalidate(mem, p.nslot * half);
  for (u32 k = 0; k < p.nslot; k++) {
    p.slot[k] = (u64 *)((u8 *)mem + k * half);
    p.used[k] = p.queued[k] = 0;
  }
  p.next = 0;
  p.open = 0;
  p.hold_head = p.hold_tail = 0;
  for (int i = 0; i < NREC; i++)
    p.rec[i].n = ~0u;
  mx_frame3_stats = (struct mx_frame3_stats){0};
  mx_dma_wait(MX_CH_GIF);
  mx_dmaq_init(&p.q, MX_CH_GIF);
  mx__frame_tex_forget();
  p.q.on_done = gif_done;
  mx_irq_intc(MX_INTC_VIF1, vif1_irq, 0);
  mx__frame3_end = frame_end;
  mx__frame3_stop = stop;
  mx__frame3_q = &p.q;
  mx__frame3_report = report;
  return 0;
}

u64 *mx_frame3_reserve(u32 qwc) {
  if (qwc > mx_frame3_room()) {
    mx_printf("mx_frame3_reserve: %u quadwords, a half has room for %u\n", qwc, mx_frame3_room());
    mx_exit(254);
  }
  if (!p.open)
    open_frame();
  if (p.pos + 2 * (qwc + 1) > p.end)
    flush();
  p.tag = p.pos;
  return p.pos + 2;
}

u32 mx_frame3_room(void) { return p.half_qw - 2; }

void mx_frame3_commit(u64 *end) {
  mx_e_cnt(p.tag, (u32)(end - p.tag - 2) / 2, 0, 0);
  p.pos = end;
}

void mx_frame3_call(const void *chain) {
  if (!p.open)
    open_frame();
  if (p.pos + 2 > p.end)
    flush();
  p.pos = mx_e_call(p.pos, chain, 0, 0, 0);
}

void mx_frame3_target(const struct mx_rt *t) {
  u64 *c = mx_frame3_reserve(5);
  mx_frame3_commit(mx_rt_e_env(c, t, 1, 1));
}
