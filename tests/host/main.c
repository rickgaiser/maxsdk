#include "test.h"

int test_failures;

int main(void) {
  test_gen_fields();
  test_pkt();
  test_emit();
  if (test_failures) {
    printf("host tests: %d FAILED\n", test_failures);
    return 1;
  }
  printf("host tests: all passed\n");
  return 0;
}
