/* Phase 8: picolibc on maxsdk. stdio (with floats), malloc, qsort, strtod,
   setjmp/longjmp, math, and exit: the end marker is printed by an atexit
   handler, so it only appears if exit() runs them. */

#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static int fails;
#define CHECK(c)                                                                           \
  do {                                                                                     \
    if (!(c)) {                                                                            \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                  \
      fails++;                                                                             \
    }                                                                                      \
  } while (0)

static int cmp(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

static jmp_buf jb;
static void jump(int v) { longjmp(jb, v); }

static void at_exit(void) {
  printf("libc: atexit handler ran, %s\n", fails ? "FAILED" : "passed");
  printf("@@PS2TEST_END@@ status=%d\n", fails ? 1 : 0);
}

int main(void) {
  atexit(at_exit);
  char buf[128];
  snprintf(buf, sizeof buf, "%d %u %x %lld %s %c", -42, 42u, 0xBEEFu, -1234567890123ll, "ok", 'z');
  printf("libc: %s\n", buf);
  CHECK(strcmp(buf, "-42 42 beef -1234567890123 ok z") == 0);
  snprintf(buf, sizeof buf, "%.3f %.2e %g", 3.14159, 12345.678, 0.5);
  printf("libc: %s\n", buf);
  CHECK(strcmp(buf, "3.142 1.23e+04 0.5") == 0);

  /* malloc: many blocks, then free them */
  void *p[100];
  for (int i = 0; i < 100; i++) {
    p[i] = malloc((size_t)(i * 37 + 1));
    CHECK(p[i] != NULL);
    memset(p[i], i, (size_t)(i * 37 + 1));
  }
  for (int i = 0; i < 100; i++)
    CHECK(((unsigned char *)p[i])[i * 37] == (unsigned char)i);
  for (int i = 0; i < 100; i++)
    free(p[i]);
  void *big = malloc(4 << 20);
  CHECK(big != NULL);
  free(big);

  int v[8] = {5, 3, 9, 1, 7, 2, 8, 6};
  qsort(v, 8, sizeof v[0], cmp);
  CHECK(v[0] == 1 && v[7] == 9 && v[3] == 5);

  double d = strtod("2.5e3", NULL);
  CHECK(d == 2500.0);
  CHECK(atoi("-77") == -77);

  int r = setjmp(jb);
  if (r == 0)
    jump(5);
  CHECK(r == 5);

  float s = sqrtf(2.0f), c = cosf(0.0f), e = expf(1.0f);
  printf("libc: sqrtf(2) %.6f cosf(0) %.6f expf(1) %.6f sin(1) %.9f\n", (double)s, (double)c,
         (double)e, sin(1.0));
  CHECK(fabsf(s - 1.414214f) < 1e-5f && c == 1.0f && fabsf(e - 2.718282f) < 1e-5f);
  CHECK(fabs(sin(1.0) - 0.841470985) < 1e-9);

  /* time from COP0 Count: 10 ms of busy waiting */
  clock_t c0 = clock();
  struct timeval t0, t1;
  gettimeofday(&t0, NULL);
  volatile unsigned spin = 0;
  clock_t c1;
  do
    spin++;
  while ((c1 = clock()) - c0 < CLOCKS_PER_SEC / 100);
  gettimeofday(&t1, NULL);
  long us = (long)(t1.tv_sec - t0.tv_sec) * 1000000 + (t1.tv_usec - t0.tv_usec);
  printf("libc: clock() 10 ms = %ld us by gettimeofday, time() %ld\n", us, (long)time(NULL));
  CHECK(us >= 9900 && us <= 10200);
  return 0;
}
