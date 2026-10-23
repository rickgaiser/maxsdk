#ifndef MX_TRACE_H
#define MX_TRACE_H

/* Event trace: COP0 Count timestamps of interrupts, DMA segment starts and
   ends, fences and waits, in a ring buffer. Off until mx_trace_enable(1).
   mx_trace_dump() prints it as "T <count> <event> <arg>" lines, which
   tools/trace.py turns into a per-unit timeline. */

#include <mx/types.h>

enum {
  MX_TR_DMAC_IRQ = 1, /* arg: channel */
  MX_TR_INTC_IRQ,     /* arg: cause */
  MX_TR_DMA_START,    /* arg: channel */
  MX_TR_DMA_END,      /* arg: channel (segment end seen by the engine) */
  MX_TR_WAIT_BEGIN,   /* arg: channel / queue */
  MX_TR_WAIT_END,
  MX_TR_GS_SIGNAL,
  MX_TR_GS_FINISH,
  MX_TR_VSYNC,
  MX_TR_SAMPLE,       /* arg: MX_TS_* bits of the busy units */
  MX_TR_USER = 0x100, /* free for programs */
};

struct mx_trace_ev {
  u32 t;
  u16 ev;
  u16 arg;
};

#define MX_TRACE_N 4096

extern int mx__trace_on;
extern u32 mx__trace_pos;
extern struct mx_trace_ev mx__trace_buf[MX_TRACE_N];

MX_INLINE void mx_trace(u16 ev, u16 arg) {
  if (!mx__trace_on)
    return;
  u32 c;
  __asm__ volatile("mfc0 %0, $9" : "=r"(c));
  struct mx_trace_ev *e = &mx__trace_buf[mx__trace_pos++ & (MX_TRACE_N - 1)];
  e->t = c;
  e->ev = ev;
  e->arg = arg;
}

void mx_trace_enable(int on);
void mx_trace_clear(void);
u32 mx_trace_count(void);
/* Copies up to n events, oldest first; returns how many. */
u32 mx_trace_get(struct mx_trace_ev *out, u32 n);
void mx_trace_dump(void);

/* ---- sampling ----------------------------------------------------------- */

/* Unit state bits, read from the hardware (mx_trace_units). */
enum {
  MX_TS_VIF0 = 1 << 0,    /* DMA channel running (CHCR.STR; an MFIFO drain: ring not empty) */
  MX_TS_VIF1 = 1 << 1,
  MX_TS_GIF = 1 << 2,
  MX_TS_FROMSPR = 1 << 3,
  MX_TS_TOSPR = 1 << 4,
  MX_TS_VU0 = 1 << 5,     /* micro program running (VPU-STAT) */
  MX_TS_VU1 = 1 << 6,
  MX_TS_PATH = 1 << 7,    /* the GIF is sending a path to the GS (GIF_STAT.APATH) */
  MX_TS_GSFIFO = 1 << 8,  /* the GS input FIFO is not empty (CSR.FIFO) */
  MX_TS_VU1GW = 1 << 9,   /* VU1 (counted busy) waits at an XGKICK for the GIF (VPU-STAT.VGW1) */
  MX_TS_N = 10
};

u32 mx_trace_units(void);

/* Sample the unit state hz times per second from timer 1 (hz = 0 stops).
   Each sample bumps a counter per busy unit, and is recorded as an
   MX_TR_SAMPLE event while the trace is on. */
void mx_trace_sample(u32 hz);
/* Samples taken since the last mx_trace_sample(), and per unit
   (busy[MX_TS_N], index = bit number). */
u32 mx_trace_busy(u32 *busy);

#endif
