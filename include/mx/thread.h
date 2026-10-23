#ifndef MX_THREAD_H
#define MX_THREAD_H

/* Threads and semaphores (phase 8), on the EE kernel's (BIOS) threads:
   fixed priorities (a lower number runs first), no time slicing, a thread
   runs until it waits, yields or a higher-priority thread wakes up.

   Fences: mx_event_sleep() puts the thread to sleep until a DMA queue
   passes the event; the queue's end interrupt wakes it (iSignalSema) only
   when a thread sleeps on it, so polling code pays nothing. One sleeping
   thread per queue. */

#include <mx/dma.h>
#include <mx/types.h>
#include <mx/vu.h>

/* Create and start a thread running fn(arg) on the stack [stack, stack +
   bytes) at priority prio (1..127; the main thread's is mx_thread_prio of
   mx_thread_self()). Returns the thread id, or < 0. Returning from fn ends
   the thread. */
int mx_thread_start(void (*fn)(void *), void *arg, void *stack, u32 bytes, int prio);
int mx_thread_self(void);
int mx_thread_prio(int id);
int mx_thread_set_prio(int id, int prio);
/* Let the other ready threads of the same priority run. */
void mx_thread_yield(void);
/* Wait until thread id has ended (sleeps, so lower-priority threads run),
   then delete it. */
void mx_thread_join(int id);

int mx_sema_create(int init, int max);
void mx_sema_delete(int s);
void mx_sema_wait(int s);
void mx_sema_signal(int s);
/* Take the semaphore if its count is above 0; returns 1 if taken. */
int mx_sema_try(int s);
/* From an interrupt handler. */
void mx_isema_signal(int s);

/* Sleep until event e of a DMA queue (mx_dmaq) has passed. */
void mx_event_sleep(mx_event e);
/* The same for a VU queue batch (single buffer, DMA): sleeps until its DMA
   has ended, then polls the batch's MARK (a few hundred cycles at most). */
int mx_vuq_sleep(struct mx_vuq *q, mx_event e);

#endif
