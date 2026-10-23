#ifndef MX_IRQ_H
#define MX_IRQ_H

/* Interrupt dispatch. Each DMAC channel and INTC cause has one SDK handler
   slot; the SDK registers a single BIOS handler per cause and calls the slot
   from it. Handlers run with interrupts disabled and may only use the i*
   BIOS calls. */

#include <mx/types.h>

typedef void (*mx_irq_fn)(void *arg);

/* Install fn for DMAC channel ch (MX_DMAC_*) and enable its interrupt.
   fn = 0 disables it. */
void mx_irq_dmac(int ch, mx_irq_fn fn, void *arg);
/* Install fn for INTC cause (MX_INTC_*) and enable it. fn = 0 disables it. */
void mx_irq_intc(int cause, mx_irq_fn fn, void *arg);

/* Number of times each slot ran (for tests and traces). */
u32 mx_irq_dmac_count(int ch);
u32 mx_irq_intc_count(int cause);

#endif
