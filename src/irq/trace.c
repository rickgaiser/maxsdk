#include <mx/cpu.h>
#include <mx/debug.h>
#include <mx/trace.h>

int mx__trace_on;
u32 mx__trace_pos;
struct mx_trace_ev mx__trace_buf[MX_TRACE_N];

void mx_trace_enable(int on) { mx__trace_on = on; }

void mx_trace_clear(void) {
  int was = mx_di();
  mx__trace_pos = 0;
  mx_irq_restore(was);
}

u32 mx_trace_count(void) { return mx__trace_pos < MX_TRACE_N ? mx__trace_pos : MX_TRACE_N; }

u32 mx_trace_get(struct mx_trace_ev *out, u32 n) {
  u32 cnt = mx_trace_count();
  u32 first = mx__trace_pos - cnt;
  if (n > cnt)
    n = cnt;
  for (u32 i = 0; i < n; i++)
    out[i] = mx__trace_buf[(first + i) & (MX_TRACE_N - 1)];
  return n;
}

void mx_trace_dump(void) {
  int on = mx__trace_on;
  mx__trace_on = 0;
  u32 cnt = mx_trace_count();
  u32 first = mx__trace_pos - cnt;
  for (u32 i = 0; i < cnt; i++) {
    struct mx_trace_ev *e = &mx__trace_buf[(first + i) & (MX_TRACE_N - 1)];
    mx_printf("T %u %u %u\n", e->t, e->ev, e->arg);
  }
  mx__trace_on = on;
}
