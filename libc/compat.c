/* The few ps2sdk EE calls that bench uses (the headers in compat/ps2sdk;
   D10a). */

#include <timer.h>

#include <mx/cpu.h>

/* Bus clock ticks (147.456 MHz) since start: COP0 Count / 2, extended to
   64 bits at each call (calls must be less than 14.5 s apart). */
u64 GetTimerSystemTime(void) {
  static u32 last, high;
  int was = mx_di();
  u32 c = mx_count();
  if (c < last)
    high++;
  last = c;
  u64 t = ((u64)high << 32 | c) >> 1;
  mx_irq_restore(was);
  return t;
}
