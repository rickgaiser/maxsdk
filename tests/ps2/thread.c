/* Phase 8: BIOS threads and semaphores, and threads that sleep on a DMA
   fence while a lower-priority thread works. */

#include "t.h"

static u8 stack[2][16 * 1024] MX_ALIGNED(16);
static u64 chain[16][2] MX_ALIGNED(64);
static u8 nops[16 * 1024] MX_ALIGNED(64);

static int sem_a, sem_b;
static volatile int value;
static volatile u32 counter, stop;

static void set_value(void *arg) {
  value = (int)(uintptr_t)arg;
  mx_sema_signal(sem_a);
}

static void basic(int main_prio) {
  sem_a = mx_sema_create(0, 1);
  value = 0;
  /* a lower priority: it runs only once main waits */
  int id = mx_thread_start(set_value, (void *)42, stack[0], sizeof stack[0], main_prio + 1);
  CHECK(id >= 0);
  CHECK_EQ(value, 0);
  mx_sema_wait(sem_a);
  CHECK_EQ(value, 42);
  mx_thread_join(id);
  /* a higher priority: it runs at once */
  value = 0;
  id = mx_thread_start(set_value, (void *)7, stack[0], sizeof stack[0], main_prio - 1);
  CHECK(id >= 0);
  CHECK_EQ(value, 7);
  mx_sema_wait(sem_a);
  mx_thread_join(id);
  mx_sema_delete(sem_a);
}

static void pong(void *arg) {
  for (int i = 0; i < 1000; i++) {
    mx_sema_wait(sem_a);
    counter++;
    mx_sema_signal(sem_b);
  }
}

static void ping_pong(int main_prio) {
  sem_a = mx_sema_create(0, 1);
  sem_b = mx_sema_create(0, 1);
  counter = 0;
  int id = mx_thread_start(pong, 0, stack[0], sizeof stack[0], main_prio);
  u32 t0 = mx_count();
  for (int i = 0; i < 1000; i++) {
    mx_sema_signal(sem_a);
    mx_sema_wait(sem_b);
  }
  u32 t = mx_count() - t0;
  CHECK_EQ(counter, 1000);
  mx_printf("thread: semaphore round trip between two threads: %u cycles\n", t / 1000);
  mx_thread_join(id);
  mx_sema_delete(sem_a);
  mx_sema_delete(sem_b);
}

static void spin(void *arg) {
  while (!stop)
    counter++;
}

/* main sleeps on the last of 16 DMA segments (VIF1, 16 KiB of NOPs each);
   a lower-priority thread counts meanwhile */
static void fence_sleep(int main_prio) {
  for (int i = 0; i < 16; i++) {
    chain[i][0] = mx_dmatag(MX_TAG_REFE, 1024, nops, 0);
    chain[i][1] = 0;
  }
  mx_dcache_writeback(chain, sizeof chain);
  static struct mx_dmaq q;
  mx_dmaq_init(&q, MX_CH_VIF1);
  counter = 0, stop = 0;
  int id = mx_thread_start(spin, 0, stack[1], sizeof stack[1], main_prio + 1);
  mx_event e = {0, 0};
  for (int i = 0; i < 16; i++)
    e = mx_dmaq_chain(&q, chain[i], 0);
  CHECK_EQ(counter, 0);
  mx_event_sleep(e);
  u32 woke = mx_count();
  CHECK(mx_event_done(e));
  u32 n = counter;
  stop = 1;
  mx_thread_join(id);
  mx_printf("thread: the worker counted to %u while main slept; DMA end to main running: %u "
            "cycles\n",
            n, woke - q.t_last_end);
  CHECK(n > 0);
  mx_dmaq_release(&q);
}

int main(void) {
  mx_dma_init();
  MX_R32(MX_VIF_VIF1_FBRST) = 1;
  int self = mx_thread_self(), prio = mx_thread_prio(self);
  mx_printf("thread: main thread %d, priority %d\n", self, prio);
  if (prio < 2 || prio > 120) {
    /* room above and below */
    mx_thread_set_prio(self, 64);
    prio = 64;
  }
  basic(prio);
  ping_pong(prio);
  fence_sleep(prio);
  return t_done("thread");
}
