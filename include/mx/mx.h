#ifndef MX_MX_H
#define MX_MX_H

/* maxsdk: everything. */

#include <mx/bios.h>
#include <mx/bits.h>
#include <mx/cpu.h>
#include <mx/debug.h>
#include <mx/dma.h>
#include <mx/emit.h>
#include <mx/frame.h>
#include <mx/gif.h>
#include <mx/gs.h>
#include <mx/hw/formats.h>
#include <mx/hw/regs.h>
#include <mx/irq.h>
#include <mx/pkt.h>
#include <mx/sched.h>
#include <mx/string.h>
#include <mx/thread.h>
#include <mx/timer.h>
#include <mx/trace.h>
#include <mx/types.h>
#include <mx/vif.h>
#include <mx/vu.h>

/* Flush debug output and return to the loader with status. */
void mx_exit(int status) __attribute__((noreturn));
void *mx_heap_start(void);
void *mx_heap_end(void);

/* End marker for the test runners (ps2-pcsx2-test, ps2-hw-test). In the
   combined batch ELF (MX_BATCH) only the batch prints it. */
#ifdef MX_BATCH
#define MX_TEST_END(status) ((void)(status))
#else
#define MX_TEST_END(status) mx_printf("@@PS2TEST_END@@ status=%d\n", (status))
#endif

#endif
