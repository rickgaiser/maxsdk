/* Phase 1: the base runtime starts, prints through every sink and exits. */

#include <mx/mx.h>

int main(int argc, char **argv) {
  mx_printf("hello from maxsdk: argc=%d argv0=%s\n", argc, argc > 0 ? argv[0] : "-");
  mx_printf("printf: %d %5d|%-5d|%05d %u %x %X %p %c %s %.3s\n", -42, 7, 7, 42, 3000000000u,
            0xbeef, 0xbeef, (void *)0x100000, 'Z', "str", "abcdef");
  mx_printf("printf: %lld %llx %f %.2f %e %g %g\n", -1234567890123ll, 0x123456789abcdefull, 3.25,
            -0.005, 12345.678, 0.0001, 100.0);
  mx_printf("heap %p..%p sinks %d\n", mx_heap_start(), mx_heap_end(), mx_debug_get_sinks());
  MX_TEST_END(0);
  return 0;
}
