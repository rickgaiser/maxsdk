/* Helpers for the PCSX2/PS2 tests. */
#ifndef T_H
#define T_H

#include <mx/mx.h>

static int t_fail;

#define CHECK(c)                                                                           \
  do {                                                                                     \
    if (!(c)) {                                                                            \
      mx_printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                               \
      t_fail++;                                                                            \
    }                                                                                      \
  } while (0)

#define CHECK_EQ(a, b)                                                                     \
  do {                                                                                     \
    unsigned long long a_ = (unsigned long long)(a), b_ = (unsigned long long)(b);         \
    if (a_ != b_) {                                                                        \
      mx_printf("FAIL %s:%d: %s = 0x%llx, expected 0x%llx\n", __FILE__, __LINE__, #a, a_, \
                b_);                                                                       \
      t_fail++;                                                                            \
    }                                                                                      \
  } while (0)

static inline int t_done(const char *name) {
  mx_printf("%s: %s\n", name, t_fail ? "FAILED" : "passed");
  MX_TEST_END(t_fail ? 1 : 0);
  return t_fail ? 1 : 0;
}

#endif
