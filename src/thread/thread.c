#include <mx/bios.h>
#include <mx/cpu.h>
#include <mx/thread.h>

/* EE kernel structures (EE Kernel Reference: CreateThread, ReferThreadStatus,
   CreateSema) */
struct thread_param {
  int status;
  void *func;
  void *stack;
  int stack_size;
  void *gp;
  int initial_priority;
  int current_priority;
  u32 attr, option;
};

struct thread_status {
  int status;
  void *func;
  void *stack;
  int stack_size;
  void *gp;
  int initial_priority;
  int current_priority;
  u32 attr, option;
  u32 wait_type, wait_id, wakeup_count;
};

struct sema_param {
  int count, max_count, init_count, wait_threads;
  u32 attr, option;
};

#define THS_DORMANT 0x10

struct start {
  void (*fn)(void *);
  void *arg;
  int ended; /* semaphore signalled when fn returns (mx_thread_join) */
};

/* thread id -> its start record (for join) */
#define MAX_THREADS 256
static struct start *started[MAX_THREADS];

static void entry(void *p) {
  struct start *s = p;
  s->fn(s->arg);
  mx_sema_signal(s->ended);
  mx_bios_exit_thread();
}

int mx_thread_start(void (*fn)(void *), void *arg, void *stack, u32 bytes, int prio) {
  /* the start record lives at the bottom of the thread's stack */
  struct start *s = stack;
  s->fn = fn;
  s->arg = arg;
  s->ended = mx_sema_create(0, 1);
  if (s->ended < 0)
    return -1;
  void *gp;
  __asm__("move %0, $gp" : "=r"(gp));
  struct thread_param tp = {0};
  tp.func = (void *)entry;
  tp.stack = (u8 *)stack + 16;
  tp.stack_size = (int)(bytes - 16) & ~15;
  tp.gp = gp;
  tp.initial_priority = prio;
  int id = mx_bios_create_thread(&tp);
  if (id < 0 || id >= MAX_THREADS)
    return -1;
  started[id] = s;
  if (mx_bios_start_thread(id, s) < 0)
    return -1;
  return id;
}

int mx_thread_self(void) { return mx_bios_get_thread_id(); }

int mx_thread_prio(int id) {
  struct thread_status st;
  if (mx_bios_refer_thread_status(id, &st) < 0)
    return -1;
  return st.current_priority;
}

int mx_thread_set_prio(int id, int prio) { return mx_bios_change_thread_priority(id, prio); }

void mx_thread_yield(void) { mx_bios_rotate_thread_ready_queue(mx_thread_prio(mx_thread_self())); }

void mx_thread_join(int id) {
  struct start *s = id >= 0 && id < MAX_THREADS ? started[id] : 0;
  if (!s)
    return;
  /* blocks, so threads of any priority can run to their end */
  mx_sema_wait(s->ended);
  mx_sema_delete(s->ended);
  /* it signalled just before ExitThread and may not have got there (a
     lower-priority thread waits behind us): end it here */
  struct thread_status st;
  if (mx_bios_refer_thread_status(id, &st) >= 0 && !(st.status & THS_DORMANT))
    mx_bios_terminate_thread(id);
  mx_bios_delete_thread(id);
  started[id] = 0;
}

int mx_sema_create(int init, int max) {
  struct sema_param sp = {0};
  sp.init_count = init;
  sp.max_count = max;
  return mx_bios_create_sema(&sp);
}

void mx_sema_delete(int s) { mx_bios_delete_sema(s); }
void mx_sema_wait(int s) { mx_bios_wait_sema(s); }
void mx_sema_signal(int s) { mx_bios_signal_sema(s); }
int mx_sema_try(int s) { return mx_bios_poll_sema(s) >= 0; }
void mx_isema_signal(int s) { mx_bios_isignal_sema(s); }

void mx_event_sleep(mx_event e) {
  struct mx_dmaq *q = e.q;
  if (mx_event_done(e))
    return;
  if (q->sema < 0)
    q->sema = mx_sema_create(0, 1);
  int was = mx_di();
  if (mx_event_done(e)) {
    mx_irq_restore(was);
    return;
  }
  /* the end interrupt signals the semaphore once the event has passed; if
     that happens before WaitSema, the count makes WaitSema return at once */
  q->sleep_seq = e.seq;
  q->sleep_sema = q->sema;
  mx_irq_restore(was);
  mx_sema_wait(q->sema);
}

int mx_vuq_sleep(struct mx_vuq *q, mx_event e) {
  if (q->dma && !q->dbuf_mode)
    mx_event_sleep(e);
  return mx_vuq_wait(q, e);
}
