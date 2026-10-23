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

#endif
