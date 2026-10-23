#ifndef MX_SCHED_H
#define MX_SCHED_H

/* mx_sched (phase 7): keeps the units busy from one EE thread, without a
   task graph. Work that runs on a unit is a task with a poll function: it
   checks its unit without waiting, and when the unit is idle it collects
   the finished work and starts the next. Every wait in the program goes
   through mx_sched (mx_sched_wait, mx_sched_frame_begin), which polls the
   tasks while it waits, so a unit gets its next batch within one poll of
   finishing the last.

   A pool hands out the items of one job (e.g. the quadwords of a stream)
   to whoever asks first: the VU workers take whole batches, the EE takes
   small chunks between polls (mx_sched_ee_pool), so the work spreads
   itself over the units by their speed. */

#include <mx/frame.h>
#include <mx/types.h>
#include <mx/vu.h>

struct mx_task {
  /* Check the unit, collect finished work, start the next. Returns 1 while
     the task has work started or left, 0 when it is idle with nothing to do. */
  int (*poll)(struct mx_task *t);
  struct mx_task *next;
  int added;
};

void mx_sched_add(struct mx_task *t);
void mx_sched_remove(struct mx_task *t);
/* Poll every task once. Returns how many are still busy. */
int mx_sched_poll(void);
/* Poll until done(arg) returns non-zero. */
void mx_sched_wait(int (*done)(void *arg), void *arg);
/* Poll until no task is busy. */
void mx_sched_drain(void);
/* mx_frame_begin, polling the tasks while it waits for the back buffer. */
int mx_sched_frame_begin(u32 r, u32 g, u32 b);

/* ---- pools ------------------------------------------------------------------ */

struct mx_pool {
  u32 next, end; /* items [next, end) are not taken yet */
};

MX_INLINE void mx_pool_init(struct mx_pool *p, u32 n) {
  p->next = 0;
  p->end = n;
}
/* Take up to max items; returns how many (0 when the pool is empty), the
   first in *first. */
MX_INLINE u32 mx_pool_take(struct mx_pool *p, u32 max, u32 *first) {
  u32 n = p->end - p->next;
  if (n > max)
    n = max;
  *first = p->next;
  p->next += n;
  return n;
}
MX_INLINE int mx_pool_empty(const struct mx_pool *p) { return p->next == p->end; }

/* ---- VU workers --------------------------------------------------------------- */

/* A task that runs a kernel over a pool of items in place: items
   [first, first + n) are at data + first * k->item_qw quadwords; each batch
   is read back to the same place. Single-buffered batches (mx_vuq_done). */
struct mx_vu_worker {
  struct mx_task task;
  struct mx_vuq *q;
  const struct mx_kernel *k;
  const void *hdr;
  u8 *data;       /* items, in memory (written back) */
  u8 *dst;        /* results: data or another array (e.g. its UCAB alias) */
  struct mx_pool *pool;
  u32 batch;      /* items per batch */
  /* in flight */
  mx_event e;
  u32 first, n;
  u32 items;      /* items done (statistics) */
  u32 batches;
};

/* q is initialised for the kernel's VU (mx_vuq_init, mx_vu_load done). */
void mx_vu_worker_init(struct mx_vu_worker *w, struct mx_vuq *q, const struct mx_kernel *k,
                       const void *hdr, void *data, void *dst, struct mx_pool *pool, u32 batch);

/* ---- the EE --------------------------------------------------------------------- */

/* Run fn(first, n, arg) on chunks of up to chunk items from pool, polling
   the tasks between chunks, until the pool is empty and the tasks are idle. */
void mx_sched_ee_pool(struct mx_pool *pool, u32 chunk, void (*fn)(u32 first, u32 n, void *arg),
                      void *arg);

#endif
