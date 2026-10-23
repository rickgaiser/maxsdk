#include <mx/bits.h>
#include <mx/cpu.h>
#include <mx/hw/formats.h>
#include <mx/hw/regs.h>
#include <mx/timer.h>

void mx_delay_cycles(u32 cycles) {
  u32 t0 = mx_count();
  while (mx_count() - t0 < cycles)
    ;
}

static const u32 tbase[3] = {MX_TIMER_T0, MX_TIMER_T1, MX_TIMER_T2};

static struct {
  mx_irq_fn fn;
  void *arg;
} tslot[3];

static void timer_irq(void *arg) {
  int t = (int)(uintptr_t)arg;
  u32 mode = tbase[t] + MX_TIMER_MODE_OFFSET;
  /* acknowledge the compare (write 1 to EQUF), keep the mode */
  MX_R32(mode) = MX_R32(mode) | (u32)MX_BIT(TIMER_MODE_EQUF);
  if (tslot[t].fn)
    tslot[t].fn(tslot[t].arg);
}

void mx_timer_periodic(int t, u32 period, mx_irq_fn fn, void *arg) {
  u32 b = tbase[t];
  MX_R32(b + MX_TIMER_MODE_OFFSET) = 0;
  tslot[t].fn = fn;
  tslot[t].arg = arg;
  if (!fn) {
    mx_irq_intc(MX_INTC_TIM0 + t, 0, 0);
    return;
  }
  MX_R32(b + MX_TIMER_COUNT_OFFSET) = 0;
  MX_R32(b + MX_TIMER_COMP_OFFSET) = period;
  mx_irq_intc(MX_INTC_TIM0 + t, timer_irq, (void *)(uintptr_t)t);
  MX_R32(b + MX_TIMER_MODE_OFFSET) =
      (u32)(MX_SET(TIMER_MODE_CLKS, 2) | MX_BIT(TIMER_MODE_ZRET) | MX_BIT(TIMER_MODE_CUE) |
            MX_BIT(TIMER_MODE_CMPE) | MX_BIT(TIMER_MODE_EQUF) | MX_BIT(TIMER_MODE_OVFF));
}
