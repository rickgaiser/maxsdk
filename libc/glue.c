/* picolibc on maxsdk (phase 8, D8c): stdio on the debug output, the heap
   from SetupHeap, exit through mx_exit. Built into libmxc.a; programs that
   use libc link crt0, their objects, libmxc.a, picolibc's libc.a, libmx.a
   and compiler-rt's builtins (Makefile, LIBC_LIBS). */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/times.h>
#include <unistd.h>

#include <mx/cpu.h>
#include <mx/debug.h>

void mx_exit(int status) __attribute__((noreturn));
void *mx_heap_start(void);
void *mx_heap_end(void);

/* ---- stdio ---------------------------------------------------------------- */

static char line[256];
static unsigned line_len;

static int out_flush(FILE *f) {
  (void)f;
  if (line_len)
    mx_debug_write(line, line_len);
  line_len = 0;
  return 0;
}

static int out_put(char c, FILE *f) {
  line[line_len++] = c;
  if (c == '\n' || line_len == sizeof line)
    out_flush(f);
  return (unsigned char)c;
}

static FILE out = FDEV_SETUP_STREAM(out_put, NULL, out_flush, _FDEV_SETUP_WRITE);
FILE *const stdin = &out;
FILE *const stdout = &out;
FILE *const stderr = &out;

ssize_t write(int fd, const void *buf, size_t n) {
  if (fd != 1 && fd != 2) {
    errno = EBADF;
    return -1;
  }
  out_flush(&out);
  mx_debug_write(buf, (unsigned)n);
  return (ssize_t)n;
}

ssize_t read(int fd, void *buf, size_t n) {
  (void)fd, (void)buf, (void)n;
  errno = EBADF;
  return -1;
}

/* ---- time --------------------------------------------------------------------- */

/* EE cycles since start: COP0 Count extended to 64 bits at each call
   (calls must be less than 14.5 s apart). There is no real-time clock:
   time() counts from 0 at program start. */
static unsigned long long cycles(void) {
  static unsigned last, high;
  int was = mx_di();
  unsigned c = mx_count();
  if (c < last)
    high++;
  last = c;
  mx_irq_restore(was);
  return (unsigned long long)high << 32 | c;
}

int gettimeofday(struct timeval *tv, void *tz) {
  (void)tz;
  unsigned long long us = cycles() / 295; /* 294.912 MHz */
  tv->tv_sec = (time_t)(us / 1000000);
  tv->tv_usec = (suseconds_t)(us % 1000000);
  return 0;
}

/* clock() uses times(): CLOCKS_PER_SEC is 1000000 */
clock_t times(struct tms *t) {
  clock_t c = (clock_t)(cycles() / 295);
  if (t) {
    t->tms_utime = c;
    t->tms_stime = t->tms_cutime = t->tms_cstime = 0;
  }
  return c;
}

/* ---- heap and exit ----------------------------------------------------------- */

void *sbrk(ptrdiff_t incr) {
  static char *brk;
  if (!brk)
    brk = (char *)(((unsigned long)mx_heap_start() + 15) & ~15ul);
  if (incr < 0 || brk + incr > (char *)mx_heap_end()) {
    errno = ENOMEM;
    return (void *)-1;
  }
  char *p = brk;
  brk += incr;
  return p;
}

void _exit(int status) {
  out_flush(&out);
  mx_exit(status);
}

/* start.c calls this instead of mx_exit when libc is linked: atexit
   handlers and C++ static destructors run, stdio is flushed. */
void mx__libc_exit(int status) { exit(status); }
