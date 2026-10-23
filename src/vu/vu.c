/* VU control. COP2 instructions the assembler only takes with -mvu0 are
   written as words, with $8/$9 as the GPR operands (VU User's Manual):
     cfc2    $8, vi28 / vi29:   0x4848e000 / 0x4848e800
     cfc2.i  $8, vi29:          0x4848e801  (interlocks with VU0's program)
     ctc2    $8, vi28/vi2/vi3:  0x48c8e000 / 0x48c81000 / 0x48c81800
     vsqi.xyzw vf2, (vi3++):    0x4be3137d
     vlqi.xyzw vf1, (vi2++):    0x4be1137c
     lqc2 vf2, 0($9):           0xd9220000
     sqc2 vf1, 0($9):           0xf9210000
     vcallms 0:                 0x4a000038
     ctc2 $8, vi27; vcallmsr:   0x48c8d800, 0x4a00d839
   They are the ones bench/vudemo used on the PS2. */

#include <mx/bios.h>
#include <mx/emit.h>
#include <mx/irq.h>
#include <mx/pkt.h>
#include <mx/vu.h>

u32 mx_vpu_stat(void) {
  u32 s;
  __asm__ volatile(".word 0x4848e800\n\tmove %0, $8" : "=r"(s) : : "$8");
  return s;
}

static u32 fbrst_get(void) {
  u32 s;
  __asm__ volatile(".word 0x4848e000\n\tmove %0, $8" : "=r"(s) : : "$8");
  return s;
}

static void fbrst_set(u32 v) {
  __asm__ volatile("move $8, %0\n\t.word 0x48c8e000\n\tsync.p" : : "r"(v) : "$8", "memory");
}

static void vif_resume(int vu) {
  MX_R32(vu ? MX_VIF_VIF1_FBRST : MX_VIF_VIF0_FBRST) = (u32)MX_BIT(VIF_FBRST_STC);
}

void mx_vu_reset(int mask) {
  u32 v = 0;
  if (mask & 1)
    v |= MX_BIT(VU_FBRST_RS0);
  if (mask & 2)
    v |= MX_BIT(VU_FBRST_RS1);
  fbrst_set(v);
  if (mask & 1) {
    MX_R32(MX_VIF_VIF0_FBRST) = (u32)MX_BIT(VIF_FBRST_RST);
    mx_vu_forget(0);
  }
  if (mask & 2) {
    MX_R32(MX_VIF_VIF1_FBRST) = (u32)MX_BIT(VIF_FBRST_RST);
    mx_vu_forget(1);
  }
}

void mx_vu_enable_tbit(int vu, int on) {
  u32 bit = (u32)(vu ? MX_BIT(VU_FBRST_TE1) : MX_BIT(VU_FBRST_TE0));
  u32 v = fbrst_get() & (u32)(MX_BIT(VU_FBRST_TE0) | MX_BIT(VU_FBRST_DE0) |
                              MX_BIT(VU_FBRST_TE1) | MX_BIT(VU_FBRST_DE1));
  fbrst_set(on ? v | bit : v & ~bit);
}

void mx_vu_enable_dbit(int vu, int on) {
  u32 bit = (u32)(vu ? MX_BIT(VU_FBRST_DE1) : MX_BIT(VU_FBRST_DE0));
  u32 v = fbrst_get() & (u32)(MX_BIT(VU_FBRST_TE0) | MX_BIT(VU_FBRST_DE0) |
                              MX_BIT(VU_FBRST_TE1) | MX_BIT(VU_FBRST_DE1));
  fbrst_set(on ? v | bit : v & ~bit);
}

/* ---- VU0 through COP2 ---------------------------------------------------- */

/* The COP2 transfers set vi2/vi3, which a running micro program uses too:
   a plain ctc2 does not wait, so first wait for VU0 to stop (cfc2.i). */
void mx_vu0_write(u32 addr, const void *src, u32 qwc) {
  mx_vu0_wait();
  __asm__ volatile("move $8, %0\n\t.word 0x48c81800" : : "r"(addr) : "$8");
  const u8 *s = src;
  for (u32 i = 0; i < qwc; i++, s += 16)
    __asm__ volatile("move $9, %0\n\t.word 0xd9220000\n\t.word 0x4be3137d"
                     :
                     : "r"(s)
                     : "$9", "memory");
}

void mx_vu0_read(void *dst, u32 addr, u32 qwc) {
  mx_vu0_wait();
  __asm__ volatile("move $8, %0\n\t.word 0x48c81000" : : "r"(addr) : "$8");
  u8 *d = dst;
  for (u32 i = 0; i < qwc; i++, d += 16)
    __asm__ volatile("move $9, %0\n\t.word 0x4be1137c\n\t.word 0xf9210000"
                     :
                     : "r"(d)
                     : "$9", "memory");
}

void mx_vu0_call(u32 addr) {
  if (addr == 0) {
    __asm__ volatile(".word 0x4a000038" ::: "memory"); /* vcallms 0 */
    return;
  }
  /* ctc2 $8, vi27 (CMSAR0); vcallmsr vi27 */
  __asm__ volatile("move $8, %0\n\t.word 0x48c8d800\n\t.word 0x4a00d839"
                   :
                   : "r"(addr)
                   : "$8", "memory");
}

void mx_vu0_wait(void) { __asm__ volatile(".word 0x4848e801" ::: "$8", "memory"); }

void mx_vu0_read_mapped(void *dst, u32 addr, u32 qwc) {
  const u8 *s = (const u8 *)(uintptr_t)MX_VU0_DATA + 16 * addr;
  u8 *d = dst;
  for (u32 i = 0; i < qwc; i++, s += 16, d += 16)
    __asm__ volatile("lq $8, 0(%1)\n\tsq $8, 0(%0)" : : "r"(d), "r"(s) : "$8", "memory");
}

void mx_vu1_read(void *dst, u32 addr, u32 qwc) {
  const u8 *s = (const u8 *)(uintptr_t)MX_VU1_DATA + 16 * addr;
  u8 *d = dst;
  for (u32 i = 0; i < qwc; i++, s += 16, d += 16)
    __asm__ volatile("lq $8, 0(%1)\n\tsq $8, 0(%0)" : : "r"(d), "r"(s) : "$8", "memory");
}

static void read_dma(u32 mem, void *dst, u32 addr, u32 qwc, void *spr, u32 bytes) {
  u32 piece = bytes / 16;
  u8 *d = dst;
  while (qwc) {
    u32 n = qwc > piece ? piece : qwc;
    MX_DMA_REG(MX_CH_TOSPR, MADR) = mem + 16 * addr;
    MX_DMA_REG(MX_CH_TOSPR, SADR) = (u32)(uintptr_t)spr & 0x3FF0u;
    MX_DMA_REG(MX_CH_TOSPR, QWC) = n;
    mx_sync();
    MX_DMA_REG(MX_CH_TOSPR, CHCR) = (u32)MX_CHCR_STR;
    mx_dma_wait(MX_CH_TOSPR);
    if (MX_GET(D_CTRL_MFD, MX_R32(MX_D_CTRL))) {
      /* fromSPR is the MFIFO's source (its MADR is the ring's write
         pointer): the EE copies from the scratchpad instead */
      const u8 *sp = spr;
      for (u32 i = 0; i < n; i++)
        __asm__ volatile("lq $8, 0(%1)\n\tsq $8, 0(%0)"
                         :
                         : "r"(d + 16 * i), "r"(sp + 16 * i)
                         : "$8", "memory");
    } else {
      mx_dma_from_spr(d, spr, n);
      mx_dma_wait(MX_CH_FROMSPR);
      mx_dcache_invalidate(d, n * 16);
    }
    d += 16 * n;
    addr += n;
    qwc -= n;
  }
}

void mx_vu0_read_dma(void *dst, u32 addr, u32 qwc, void *spr, u32 bytes) {
  read_dma(MX_VU0_DATA, dst, addr, qwc, spr, bytes);
}

void mx_vu1_read_dma(void *dst, u32 addr, u32 qwc, void *spr, u32 bytes) {
  read_dma(MX_VU1_DATA, dst, addr, qwc, spr, bytes);
}

/* ---- microcode ------------------------------------------------------------ */

static struct {
  const void *code;
  u32 bytes, addr;
} resident[2][4];

void mx_vu_forget(int vu) {
  for (int i = 0; i < 4; i++)
    resident[vu][i].code = 0;
}

static int is_resident(int vu, const void *code, u32 bytes, u32 addr) {
  for (int i = 0; i < 4; i++)
    if (resident[vu][i].code == code && resident[vu][i].bytes == bytes &&
        resident[vu][i].addr == addr)
      return 1;
  return 0;
}

static void mark_resident(int vu, const void *code, u32 bytes, u32 addr) {
  /* drop what the new code overlaps */
  int free = -1;
  for (int i = 0; i < 4; i++) {
    u32 a = resident[vu][i].addr, e = a + resident[vu][i].bytes / 8;
    if (resident[vu][i].code && a < addr + bytes / 8 && addr < e)
      resident[vu][i].code = 0;
    if (!resident[vu][i].code && free < 0)
      free = i;
  }
  if (free < 0)
    free = 0;
  resident[vu][free].code = code;
  resident[vu][free].bytes = bytes;
  resident[vu][free].addr = addr;
}

static u32 mpg_buf[2 * 2048 + 64] MX_ALIGNED(64);
static u64 mpg_tags[2 * 16] MX_ALIGNED(64);

u64 *mx_vu_mpg_tags(u64 *t, const void *code, u32 bytes, u32 addr) {
  u32 n = bytes / 8;
  const u32 *c = code;
  while (n >= 2) {
    u32 k = n > 256 ? 256 : n & ~1u;
    t = mx_e_ref(t, c, k / 2, 0, mx_vif_mpg(k, addr));
    c += 2 * k;
    addr += k;
    n -= k;
  }
  if (n) {
    t = mx_e_cnt(t, 1, 0, mx_vif_mpg(1, addr));
    t = mx_e_w4(t, c[0], c[1], 0, 0);
  }
  return t;
}

int mx_vu_resident(int vu, const void *code, u32 bytes, u32 addr) {
  return is_resident(vu, code, bytes, addr);
}

void mx_vu_mark_resident(int vu, const void *code, u32 bytes, u32 addr) {
  mark_resident(vu, code, bytes, addr);
}

/* The code is sent where it is, by ref tags with MPG in the tag's upper
   word (the instructions then start on a quadword). An odd last instruction
   goes in a cnt packet of its own, padded with VIF NOPs. Code that is not
   16-byte aligned is copied into a packet with the builder. */
static int load_chain(const void *code, u32 bytes, u32 addr, void **chain) {
  u32 n = bytes / 8;
  if ((uintptr_t)code & 15) {
    struct mx_pkt p;
    mx_pkt_begin(&p, mpg_buf, sizeof mpg_buf, MX_PKT_VIF);
    mx_pkt_end(&p, mx_vif_flushe(), 0);
    mx_pkt_mpg(&p, code, bytes, addr);
    if (mx_pkt_finish(&p) < 0)
      return -1;
    mx_pkt_writeback(&p);
    *chain = mpg_buf;
    return 0;
  }
  if (n > 2048)
    return -1;
  u64 *t = mx_e_cnt(mpg_tags, 0, mx_vif_flushe(), 0);
  t = mx_vu_mpg_tags(t, code, bytes, addr);
  t = mx_e_end(t, 0, 0, 0);
  mx_dcache_writeback(mpg_tags, (u32)((u8 *)t - (u8 *)mpg_tags));
  *chain = mpg_tags;
  return 0;
}

int mx_vu_load(int vu, const void *code, u32 bytes, u32 addr) {
  if (is_resident(vu, code, bytes, addr))
    return 0;
  int ch = vu ? MX_CH_VIF1 : MX_CH_VIF0;
  /* tags written cached and written back: VIF0 did not start on the PS2 with
     tags written through the uncached alias (docs/dma.md) */
  mx_dma_wait(ch);
  void *chain;
  if (load_chain(code, bytes, addr, &chain) < 0)
    return -1;
  mx_dma_chain(ch, chain, 1);
  mx_dma_wait(ch);
  mark_resident(vu, code, bytes, addr);
  return 1;
}

/* ---- kernels and batches ------------------------------------------------ */

static u32 buf_qw(const struct mx_kernel *k) { return ((k->vu ? 1024 : 256) - k->data_qw) / 2; }

u32 mx_kernel_max_items(const struct mx_kernel *k) {
  u32 mem = k->vu ? 1024 : 256;
  if (k->dbuf)
    return (buf_qw(k) - 1) / k->item_qw;
  return (mem - k->data_qw) / k->item_qw;
}

u32 mx_kernel_buffer(const struct mx_kernel *k, int b) { return k->data_qw + (u32)b * buf_qw(k); }

void mx_vuq_init(struct mx_vuq *q, int vu, int dma) {
  q->vu = vu;
  q->dma = vu ? 1 : dma;
  q->slot = 0;
  q->spr = 0;
  q->spr_bytes = 0;
  q->db_started = 0;
  q->ready_seq = 0;
  q->hdr_valid = 0;
  q->dbuf_mode = 0;
  q->dbuf_irq = 0;
  q->stalls = q->stalled_seq = q->read_seq = 0;
  q->last_db = 0;
  q->db_n = 0;
  for (int i = 0; i < MX_VUQ_SLOTS; i++)
    q->prev_of[i] = 0;
  /* batch seq ends with VIF MARK (mark_base + seq): MARK values of earlier
     users of the VIF are not mistaken for ours */
  q->mark_base = MX_R32(vu ? MX_VIF_VIF1_MARK : MX_VIF_VIF0_MARK) + 0x100;
  if (q->dma)
    mx_dmaq_init(&q->dq, vu ? MX_CH_VIF1 : MX_CH_VIF0);
  else {
    q->dq.ch = -1;
    q->dq.done = q->dq.issued = 0;
    q->dq.sleep_sema = q->dq.sema = -1;
  }
}

void mx_vuq_release(struct mx_vuq *q) {
  if (q->dbuf_irq) {
    if (q->stalled_seq) {
      q->stalled_seq = 0;
      vif_resume(q->vu);
    }
    mx_irq_intc(q->vu ? MX_INTC_VIF1 : MX_INTC_VIF0, 0, 0);
    q->dbuf_irq = 0;
  }
  if (q->dma)
    mx_dmaq_release(&q->dq);
}

static int k_dbuf(const struct mx_vuq *q) { return q->dbuf_mode; }

/* The VIF stopped at the i-bit before batch seq's MSCAL: go on at once if
   the previous batch's results have been read, else hold until they are. */
static void vif_ibit_irq(void *arg) {
  struct mx_vuq *q = arg;
  u32 seq = ++q->stalls;
  u32 prev = q->prev_of[seq % MX_VUQ_SLOTS];
  if (!prev || (s32)(q->read_seq - prev) >= 0)
    vif_resume(q->vu);
  else
    q->stalled_seq = seq;
}

static int vif_idle(int vu) {
  u32 stat = vu ? MX_VIF_VIF1_STAT : MX_VIF_VIF0_STAT;
  u32 busy = vu ? MX_VPU_VBS1 : MX_VPU_VBS0;
  return !(MX_R32(stat) & 0x1F000003u) && !(mx_vpu_stat() & busy);
}

/* A double-buffered batch: [FLUSHE + header if it changed] [VU0: ITOP base]
   count + items to the buffer, MSCAL, MARK seq. No FLUSHE: the DMA ends
   while the program runs, and the next batch goes to the other buffer. */
static mx_event batch_dbuf(struct mx_vuq *q, const struct mx_kernel *k, const void *hdr,
                           const void *data, u32 n) {
  int vu = q->vu;
  u32 seq = q->dq.issued + 1;
  /* buffers alternate per batch (MSCAL swaps VIF1's TOPS); a terminator
     (wait_dbuf) takes a seq but no buffer */
  int b = (int)(q->db_n++ & 1);
  q->buf_of[seq % MX_VUQ_SLOTS] = (u8)b;
  u32 s = q->slot++ % MX_VUQ_SLOTS;
  int unc = vu == 1;
  if (vu && !q->db_started) {
    /* known double-buffer state: reset VIF1 (DBF = 0, TOPS = BASE) */
    mx_dmaq_drain(&q->dq);
    while (!vif_idle(1))
      ;
    MX_R32(MX_VIF_VIF1_FBRST) = (u32)MX_BIT(VIF_FBRST_RST);
    q->hdr_valid = 0;
  }
  struct mx_pkt p;
  mx_pkt_begin(&p, unc ? MX_UNCACHED(&q->tags[s][0][0]) : &q->tags[s][0][0], sizeof q->tags[s],
               MX_PKT_VIF);
  /* the header, only when it changed (FLUSHE: the running program reads it) */
  const u32 *src = hdr;
  int same = q->hdr_valid;
  for (u32 i = 0; same && i < k->hdr_qw * 4; i++)
    same = q->last_hdr[i] == src[i];
  if (!same) {
    u32 *h = unc ? (u32 *)MX_UNCACHED(&q->hdr[s][0]) : (u32 *)&q->hdr[s][0];
    for (u32 i = 0; i < k->hdr_qw * 4; i++)
      h[i] = q->last_hdr[i] = src[i];
    q->hdr_valid = 1;
    mx_pkt_ref(&p, &q->hdr[s][0], k->hdr_qw, mx_vif_flushe(),
               mx_vif_unpack(MX_UNPACK_V4_32, 0, k->hdr_qw, 0));
    if (!unc)
      mx_dcache_writeback(&q->hdr[s][0], k->hdr_qw * 16);
  }
  u32 *c = unc ? (u32 *)MX_UNCACHED(&q->cnt[s]) : (u32 *)&q->cnt[s];
  c[0] = n, c[1] = c[2] = c[3] = 0;
  if (!unc)
    mx_dcache_writeback(&q->cnt[s], 16);
  u32 base = mx_kernel_buffer(k, b);
  u32 flg = vu ? MX_UNPACK_FLG : 0;
  u32 at = vu ? 0 : base; /* VU1: relative to TOPS */
  if (vu && !q->db_started) {
    mx_pkt_cnt(&p, mx_vif_base(k->data_qw), mx_vif_offset(buf_qw(k)));
    q->db_started = 1;
  }
  /* VU0: ITOP is taken over by the next MSCAL, so xitop gives this buffer */
  mx_pkt_cnt(&p, mx_vif_stcycl(1, 1), vu ? 0 : mx_vif_itop(base));
  mx_pkt_ref(&p, &q->cnt[s], 1, 0, mx_vif_unpack(MX_UNPACK_V4_32, at, 1, flg));
  const u8 *d = data;
  u32 items_qw = n * k->item_qw;
  at++;
  while (items_qw) {
    u32 m = items_qw > 256 ? 256 : items_qw;
    mx_pkt_ref(&p, d, m, 0, mx_vif_unpack(MX_UNPACK_V4_32, at, m, flg));
    d += 16 * m;
    at += m;
    items_qw -= m;
  }
  q->prev_of[seq % MX_VUQ_SLOTS] = q->last_db;
  q->last_db = seq;
  if (!q->dbuf_irq) {
    q->dbuf_irq = 1;
    q->stalls = q->dq.issued; /* numbered like the batches */
    mx_irq_intc(vu ? MX_INTC_VIF1 : MX_INTC_VIF0, vif_ibit_irq, q);
  }
  /* stop (i-bit) before MSCAL until the previous results are read; MARK
     after MSCAL: set once this program has started */
  mx_pkt_end(&p, mx_vif_nop() | MX_VIF_IBIT, mx_vif_mscal(k->entry));
  mx_pkt_vif(&p, mx_vif_mark((q->mark_base + seq) & 0xFFFF));
  mx_pkt_finish(&p);
  if (!unc)
    mx_pkt_writeback(&p);
  return mx_dmaq_chain(&q->dq, &q->tags[s][0][0], 1);
}

mx_event mx_vuq_batch(struct mx_vuq *q, const struct mx_kernel *k, const void *hdr,
                      const void *data, u32 n) {
  q->dbuf_mode = k->dbuf && q->dma;
  if (q->dbuf_mode)
    return batch_dbuf(q, k, hdr, data, n);
  u32 items_qw = n * k->item_qw;
  if (!q->dma) {
    /* VU0 through COP2: the previous program must have ended (CFC2.I in
       mx_vuq_wait, or here) */
    mx_vu0_wait();
    u32 h[16 * 4] MX_ALIGNED(16);
    const u32 *src = hdr;
    for (u32 i = 0; i < k->hdr_qw * 4; i++)
      h[i] = src[i];
    h[k->count_qw * 4] = n;
    mx_vu0_write(0, h, k->hdr_qw);
    mx_vu0_write(k->data_qw, data, items_qw);
    mx_vu0_call(k->entry);
    mx_event e = {&q->dq, ++q->dq.issued};
    return e;
  }
  u32 s = q->slot++ % MX_VUQ_SLOTS;
  /* The header goes into our own copy (the caller may change hdr before the
     DMA has read it), with the tags. VIF1: written uncached. VIF0: written
     cached and written back, since VIF0 did not start on the PS2 with tags
     written uncached (docs/dma.md). */
  int unc = q->vu == 1;
  u32 *h = unc ? (u32 *)MX_UNCACHED(&q->hdr[s][0]) : (u32 *)&q->hdr[s][0];
  const u32 *src = hdr;
  for (u32 i = 0; i < k->hdr_qw * 4; i++)
    h[i] = src[i];
  h[k->count_qw * 4] = n;

  struct mx_pkt p;
  mx_pkt_begin(&p, unc ? MX_UNCACHED(&q->tags[s][0][0]) : &q->tags[s][0][0], sizeof q->tags[s],
               MX_PKT_VIF);
  mx_pkt_ref(&p, &q->hdr[s][0], k->hdr_qw, mx_vif_stcycl(1, 1),
             mx_vif_unpack(MX_UNPACK_V4_32, 0, k->hdr_qw, 0));
  const u8 *d = data;
  u32 addr = k->data_qw;
  while (items_qw) {
    u32 m = items_qw > 256 ? 256 : items_qw;
    mx_pkt_ref(&p, d, m, 0, mx_vif_unpack(MX_UNPACK_V4_32, addr, m, 0));
    d += 16 * m;
    addr += m;
    items_qw -= m;
  }
  /* MARK after FLUSHE: the VIF sets it once the program has ended */
  mx_pkt_end(&p, mx_vif_mscal(k->entry), mx_vif_flushe());
  mx_pkt_vif(&p, mx_vif_mark((q->mark_base + q->dq.issued + 1) & 0xFFFF));
  mx_pkt_finish(&p);
  if (!unc) {
    mx_dcache_writeback(&q->hdr[s][0], k->hdr_qw * 16);
    mx_pkt_writeback(&p);
  }
  return mx_dmaq_chain(&q->dq, &q->tags[s][0][0], 1);
}

/* Double buffer: batch k has ended when the VIF holds batch k+1 before its
   MSCAL and the VU is idle. For the newest batch, let it start and send a
   terminator (FLUSHE, MARK) behind it. */
static int wait_dbuf(struct mx_vuq *q, mx_event e, u32 t0) {
  u32 busy = q->vu ? MX_VPU_VBS1 : MX_VPU_VBS0;
  u32 mark = q->vu ? MX_VIF_VIF1_MARK : MX_VIF_VIF0_MARK;
  if (q->last_db != e.seq) {
    for (;;) {
      u32 st = q->stalled_seq;
      if (st && q->prev_of[st % MX_VUQ_SLOTS] == e.seq && !(mx_vpu_stat() & busy))
        return 0;
      if (mx_count() - t0 > 294912000u)
        return -1;
    }
  }
  if (q->stalled_seq == e.seq) {
    q->stalled_seq = 0;
    vif_resume(q->vu);
  }
  u32 tv = (q->mark_base + e.seq + 0x4000) & 0xFFFF;
  u32 s = e.seq & 1;
  struct mx_pkt p;
  int unc = q->vu == 1;
  mx_pkt_begin(&p, unc ? MX_UNCACHED(&q->term_tags[s][0]) : &q->term_tags[s][0],
               sizeof q->term_tags[s], MX_PKT_VIF);
  mx_pkt_end(&p, mx_vif_flushe(), mx_vif_mark(tv));
  mx_pkt_finish(&p);
  if (!unc)
    mx_pkt_writeback(&p);
  /* the terminator is a batch of its own for the queue's numbering: keep
     the i-bit numbering in step */
  q->stalls++;
  mx_dmaq_chain(&q->dq, &q->term_tags[s][0], 1);
  while (MX_R32(mark) != tv)
    if (mx_count() - t0 > 294912000u)
      return -1;
  return 0;
}

int mx_vuq_wait(struct mx_vuq *q, mx_event e) {
  u32 t0 = mx_count();
  q->ready_seq = e.seq;
  if (!q->dma) {
    mx_vu0_wait();
    q->dq.done = e.seq;
    return 0;
  }
  if (q->dbuf_mode)
    return wait_dbuf(q, e, t0);
  /* Polled, not the DMA end interrupt: the MARK follows the batch's last
     quadword, so it also means the DMA is done with the batch.
     The DMA end does not mean the VIF has seen everything: the last
     quadwords can still be on their way while the VIF looks idle. Use the
     batch's MARK. Single buffer: MARK seq follows FLUSHE, so the program has
     ended. Double buffer: MARK seq follows MSCAL (the program has started);
     it has ended when MARK seq+1 is set or the VU is no longer busy. */
  u32 mark = q->vu ? MX_VIF_VIF1_MARK : MX_VIF_VIF0_MARK;
  u32 busy = q->vu ? MX_VPU_VBS1 : MX_VPU_VBS0;
  u32 want = q->mark_base + e.seq;
  for (;;) {
    s16 d = (s16)(MX_R32(mark) - want);
    if (d >= 1 || (d == 0 && (!k_dbuf(q) || !(mx_vpu_stat() & busy))))
      return 0;
    if (mx_count() - t0 > 294912000u)
      return -1;
  }
}

int mx_vuq_done(struct mx_vuq *q, mx_event e) {
  if (!q->dma)
    return !(mx_vpu_stat() & MX_VPU_VBS0);
  if (q->dbuf_mode)
    return -1;
  u32 mark = q->vu ? MX_VIF_VIF1_MARK : MX_VIF_VIF0_MARK;
  return (s16)(MX_R32(mark) - (q->mark_base + e.seq)) >= 0;
}

void mx_vuq_readback_spr(struct mx_vuq *q, void *spr, u32 bytes) {
  q->spr = spr;
  q->spr_bytes = bytes;
}

void mx_vuq_read(struct mx_vuq *q, const struct mx_kernel *k, void *dst, u32 n) {
  u32 at = k->data_qw;
  if (k->dbuf && q->dma)
    at = mx_kernel_buffer(k, q->buf_of[q->ready_seq % MX_VUQ_SLOTS]) + 1;
  if (q->spr)
    (q->vu ? mx_vu1_read_dma : mx_vu0_read_dma)(dst, at, n * k->item_qw, q->spr, q->spr_bytes);
  else if (q->vu)
    mx_vu1_read(dst, at, n * k->item_qw);
  else if (k->dbuf && q->dma)
    /* the next batch is held before its MSCAL; COP2 would change vi2 if it
       were not, so read through the mapping */
    mx_vu0_read_mapped(dst, at, n * k->item_qw);
  else
    mx_vu0_read(dst, at, n * k->item_qw);
  if (k->dbuf && q->dma) {
    q->read_seq = q->ready_seq;
    u32 st = q->stalled_seq;
    if (st && q->prev_of[st % MX_VUQ_SLOTS] == q->ready_seq) {
      q->stalled_seq = 0;
      vif_resume(q->vu);
    }
  }
}
