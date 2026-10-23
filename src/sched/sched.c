#include <mx/sched.h>
#include <mx/trace.h>

static struct mx_task *tasks;

void mx_sched_add(struct mx_task *t) {
  if (t->added)
    return;
  t->next = tasks;
  tasks = t;
  t->added = 1;
}

void mx_sched_remove(struct mx_task *t) {
  for (struct mx_task **p = &tasks; *p; p = &(*p)->next)
    if (*p == t) {
      *p = t->next;
      break;
    }
  t->added = 0;
}

int mx_sched_poll(void) {
  int busy = 0;
  for (struct mx_task *t = tasks; t; t = t->next)
    busy += t->poll(t) != 0;
  return busy;
}

void mx_sched_wait(int (*done)(void *), void *arg) {
  mx_trace(MX_TR_WAIT_BEGIN, 0x200);
  while (!done(arg))
    mx_sched_poll();
  mx_trace(MX_TR_WAIT_END, 0x200);
}

void mx_sched_drain(void) {
  while (mx_sched_poll())
    ;
}

static int frame_ready(void *arg) {
  (void)arg;
  return mx_frame_ready();
}

int mx_sched_frame_begin(u32 r, u32 g, u32 b) {
  if (!mx_frame_ready()) {
    u32 t0 = mx_count();
    mx_sched_wait(frame_ready, 0);
    mx_frame_stats.wait_buffer += mx_count() - t0;
  }
  return mx_frame_begin(r, g, b);
}

/* ---- VU workers ---------------------------------------------------------------- */

static int vu_worker_poll(struct mx_task *t) {
  struct mx_vu_worker *w = (struct mx_vu_worker *)t;
  if (w->n) {
    if (!mx_vuq_done(w->q, w->e))
      return 1;
    mx_vuq_wait(w->q, w->e); /* returns at once; marks the batch as read next */
    mx_vuq_read(w->q, w->k, w->dst + 16 * w->k->item_qw * w->first, w->n);
    w->items += w->n;
    w->n = 0;
  }
  u32 first, n = mx_pool_take(w->pool, w->batch, &first);
  if (!n)
    return 0;
  w->first = first;
  w->n = n;
  w->batches++;
  w->e = mx_vuq_batch(w->q, w->k, w->hdr, w->data + 16 * w->k->item_qw * first, n);
  return 1;
}

void mx_vu_worker_init(struct mx_vu_worker *w, struct mx_vuq *q, const struct mx_kernel *k,
                       const void *hdr, void *data, void *dst, struct mx_pool *pool, u32 batch) {
  w->task.poll = vu_worker_poll;
  w->task.added = 0;
  w->q = q;
  w->k = k;
  w->hdr = hdr;
  w->data = data;
  w->dst = dst ? dst : data;
  w->pool = pool;
  w->batch = batch;
  w->n = 0;
  w->items = w->batches = 0;
}

/* ---- the EE ------------------------------------------------------------------------ */

void mx_sched_ee_pool(struct mx_pool *pool, u32 chunk, void (*fn)(u32, u32, void *), void *arg) {
  for (;;) {
    int busy = mx_sched_poll();
    u32 first, n = mx_pool_take(pool, chunk, &first);
    if (n)
      fn(first, n, arg);
    else if (!busy)
      return;
  }
}
