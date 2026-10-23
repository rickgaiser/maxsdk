#ifndef TEST_H
#define TEST_H

#include <stdio.h>

extern int test_failures;

#define CHECK_EQ(a, b)                                                                     \
  do {                                                                                     \
    unsigned long long a_ = (unsigned long long)(a), b_ = (unsigned long long)(b);         \
    if (a_ != b_) {                                                                        \
      printf("%s:%d: %s = 0x%llx, expected 0x%llx\n", __FILE__, __LINE__, #a, a_, b_);     \
      test_failures++;                                                                     \
    }                                                                                      \
  } while (0)

void test_gen_fields(void);
void test_pkt(void);
void test_emit(void);

#endif
