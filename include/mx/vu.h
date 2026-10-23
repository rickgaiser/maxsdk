#ifndef MX_VU_H
#define MX_VU_H

/* VU0 and VU1: reset, macro/micro mode control, microcode upload (with
   residency tracking), and batches of a kernel through a queue.

   What works on the PS2 (docs/vu0.md, docs/vu1.md): VU1 is fed by VIF1 DMA
   (UNPACK, MSCAL, FLUSHE), never by writes to its mapped memory; VU0 data
   goes through COP2 (lqc2 + vsqi in, vlqi + sqc2 out) or VIF0 DMA, VCALLMS
   starts it and CFC2.I waits. Results of VU1 are read from its mapped
   memory after it has stopped.

   The COP2 transfer functions use vf1, vf2, vi2 and vi3 of VU0. */

#include <mx/dma.h>
#include <mx/types.h>

/* VPU-STAT (COP2 control register vi29) */
u32 mx_vpu_stat(void);
#define MX_VPU_VBS0 (1u << 0) /* VU0 busy */
#define MX_VPU_VBS1 (1u << 8) /* VU1 busy */

/* Reset VU0 and/or VU1 (FBRST) and their VIFs. mask: bit 0 VU0, bit 1 VU1. */
void mx_vu_reset(int mask);
/* Enable the T-bit / D-bit interrupts of VU0 (vu 0) or VU1 (vu 1). A reset
   (mx_vu_reset) clears both enables. Do not enable the D bit on VU1: a D
   bit with FBRST.DE1 set hung the PS2 (docs/interrupts.md). */
void mx_vu_enable_tbit(int vu, int on);
void mx_vu_enable_dbit(int vu, int on);

/* ---- VU0 through COP2 ---------------------------------------------------- */

/* Copy qwc quadwords to / from VU0 data memory at quadword address addr.
   They first wait for VU0's micro program to end: they use vi2/vi3. */
void mx_vu0_write(u32 addr, const void *src, u32 qwc);
void mx_vu0_read(void *dst, u32 addr, u32 qwc);
/* Read VU0 data memory through its mapping (0x1100_4000); touches no VU0
   register, so it can run next to a micro program on the other buffer. */
void mx_vu0_read_mapped(void *dst, u32 addr, u32 qwc);
/* Start VU0's micro program at addr (64-bit units) and return. */
void mx_vu0_call(u32 addr);
/* Wait for the end of VU0's micro program (CFC2.I interlocks). */
void mx_vu0_wait(void);

/* Copy qwc quadwords from VU1 data memory (mapped; VU1 must be stopped). */
void mx_vu1_read(void *dst, u32 addr, u32 qwc);
/* The same by DMA: toSPR from VU1 memory into the scratchpad buffer spr
   (bytes long), then fromSPR to dst, in pieces. ~5.5 instead of ~26 EE
   cycles per quadword (docs/vu1.md). dst is invalidated if cached. While
   the MFIFO runs (it owns fromSPR), the EE copies from the scratchpad. */
void mx_vu1_read_dma(void *dst, u32 addr, u32 qwc, void *spr, u32 bytes);
/* The same for VU0 data memory (VU0 stopped). Touches no VU0 register. */
void mx_vu0_read_dma(void *dst, u32 addr, u32 qwc, void *spr, u32 bytes);

/* ---- microcode ------------------------------------------------------------ */

/* Upload code (bytes, 8 per instruction) to micro memory addr (64-bit units)
   of vu, unless it is already there, by MPG through VIF0/VIF1. 16-byte
   aligned code (.vutext) is sent where it is by ref tags, so it must be in
   memory, not only in the cache (startup flushes the cache); other code is
   copied into a packet. Returns 1 if it was uploaded, -1 on error. */
int mx_vu_load(int vu, const void *code, u32 bytes, u32 addr);
/* The same upload as DMA tags for a VIF chain with TTE (for example an
   MFIFO stream): ref tags to the code, 256 instructions each, and an odd
   last instruction in a cnt packet. code must be 16-byte aligned and in
   memory. Writes at most bytes / 2048 + 3 quadwords; returns the cursor. */
u64 *mx_vu_mpg_tags(u64 *c, const void *code, u32 bytes, u32 addr);
/* Forget what is resident (after a reset or a foreign upload). */
void mx_vu_forget(int vu);
/* Residency for uploads made outside mx_vu_load (mx_vu_mpg_tags). */
int mx_vu_resident(int vu, const void *code, u32 bytes, u32 addr);
void mx_vu_mark_resident(int vu, const void *code, u32 bytes, u32 addr);

/* ---- kernels and batches ------------------------------------------------ */

/* A micro-mode kernel and its data memory layout: a header of hdr_qw
   quadwords (parameters, with the item count as an i32 in the x field of
   quadword count_qw) at address 0, then the items from data_qw. Items are
   transformed in place (the vuk convention).

   dbuf = 1: double buffered. The data memory from data_qw is split into two
   buffers; batches go to them in turn, each buffer starting with a count
   quadword (x) followed by the items. The kernel finds its buffer with
   xitop (VU0: the queue sends ITOP) or xtop (VU1: VIF1 BASE/OFFSET, UNPACK
   with FLG). The header is resident: it is only sent (after a FLUSHE) when
   it changes. Batch k+1's data is unpacked while batch k runs. VU memory
   must not be read while the VU runs (wrong data on the PS2), so batch k+1
   stops the VIF (i-bit) before its MSCAL until batch k's results have been
   read; mx_vuq_read lets it go on. Call mx_vuq_wait/mx_vuq_read for every
   batch, in order. A queue is used for double-buffered batches only. */
struct mx_kernel {
  int vu;
  const void *code;
  u32 code_bytes;
  u32 entry;    /* 64-bit units */
  u32 hdr_qw;   /* quadwords of parameters at address 0 */
  u32 count_qw; /* quadword of the header that holds the item count */
  u32 data_qw;  /* first item */
  u32 item_qw;  /* quadwords per item */
  int dbuf;     /* double buffered (see above) */
  u32 out_qw;   /* output quadwords per item after the items (0: in place) */
};

/* A kernel from a vuk -H header: the layout (VUK_<NAME>_N, _X, _SIZE) and
   how to launch it (_ENTRY, _ITEM_QW, _DBUF). Items are sent as UNPACK
   V4-32, the only format the queues use (_FORMAT 0x0C). */
#define MX_VUK_KERNEL(NAME, vu_, code_, code_end_)                                         \
  ((struct mx_kernel){.vu = (vu_), .code = (code_), .code_bytes = (u32)((code_end_) - (code_)), \
                      .entry = VUK_##NAME##_ENTRY, .hdr_qw = VUK_##NAME##_SIZE,             \
                      .count_qw = VUK_##NAME##_N, .data_qw = VUK_##NAME##_X,                \
                      .item_qw = VUK_##NAME##_ITEM_QW, .dbuf = VUK_##NAME##_DBUF})

/* Items per batch that fit the data memory of the kernel's VU (VU0 4 KiB,
   VU1 16 KiB), per buffer for double-buffered kernels. */
u32 mx_kernel_max_items(const struct mx_kernel *k);
/* First quadword (the count) of double buffer b of a kernel. */
u32 mx_kernel_buffer(const struct mx_kernel *k, int b);

#define MX_VUQ_SLOTS 4
#define MX_VUQ_TAGS 8

/* A queue of batches for one VU. VU1 always uses VIF1 DMA; VU0 uses COP2
   transfers (dma = 0) or VIF0 DMA (dma = 1). */
struct mx_vuq {
  int vu, dma;
  struct mx_dmaq dq;
  u32 slot;
  void *spr; /* scratchpad buffer for DMA readback of VU1, or 0 */
  u32 spr_bytes;
  /* double buffering */
  int db_started;   /* VIF1 BASE/OFFSET set, buffers known */
  u32 ready_seq;    /* the batch of the last mx_vuq_wait */
  int hdr_valid;
  int dbuf_mode;    /* the batches are double buffered */
  int dbuf_irq;             /* VIF i-bit handler installed */
  volatile u32 stalls;      /* i-bit stops seen (= seq of the stopped batch) */
  volatile u32 stalled_seq; /* batch held before its MSCAL, 0 if none */
  volatile u32 read_seq;    /* results read up to this batch */
  u32 last_db;              /* newest double-buffered batch */
  u32 prev_of[MX_VUQ_SLOTS]; /* the batch before batch seq (by seq % slots) */
  u32 db_n;                 /* double-buffered batches sent */
  u8 buf_of[MX_VUQ_SLOTS];  /* buffer of batch seq */
  u32 term[4] MX_ALIGNED(16);
  u64 term_tags[2][2] MX_ALIGNED(64);
  u32 mark_base;    /* batch seq ends with VIF MARK mark_base + seq */
  u32 last_hdr[16 * 4];
  u128 cnt[MX_VUQ_SLOTS] MX_ALIGNED(16);
  u64 tags[MX_VUQ_SLOTS][MX_VUQ_TAGS][2] MX_ALIGNED(64);
  u128 hdr[MX_VUQ_SLOTS][16] MX_ALIGNED(64);
};

void mx_vuq_init(struct mx_vuq *q, int vu, int dma);
void mx_vuq_release(struct mx_vuq *q);
/* Send one batch: the header (hdr_qw quadwords; the count is written into
   it) and n items from data (in memory, not only in the cache), then start
   the kernel. Returns at once. The previous batch's results must have been
   read (the new data overwrites them). */
mx_event mx_vuq_batch(struct mx_vuq *q, const struct mx_kernel *k, const void *hdr,
                      const void *data, u32 n);
/* Wait until the batch's data has arrived and its program has ended (for a
   double-buffered kernel the next batch may already run). Returns 0, or -1
   after a second. */
int mx_vuq_wait(struct mx_vuq *q, mx_event e);
/* Without waiting: 1 if the batch's program has ended (single-buffered
   batches; mx_vuq_wait then returns at once), 0 if not, -1 for double-
   buffered batches (use mx_vuq_wait). */
int mx_vuq_done(struct mx_vuq *q, mx_event e);
/* Read n items of results of the batch last passed to mx_vuq_wait. */
void mx_vuq_read(struct mx_vuq *q, const struct mx_kernel *k, void *dst, u32 n);
/* Read results back by DMA through this scratchpad buffer (VU1 and VU0
   queues; spr = 0: mapped or COP2 reads, the default). */
void mx_vuq_readback_spr(struct mx_vuq *q, void *spr, u32 bytes);

#endif
