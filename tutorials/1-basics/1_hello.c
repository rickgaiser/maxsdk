/* Tutorial 1, step 1: a program that prints a few lines and ends.

   mx_printf goes to the debug console: PCSX2's EE console, or ps2client's
   terminal on a PS2. The EE's cycle counter (mx_count, 294.912 MHz) times
   things. MX_TEST_END prints the line that test runners wait for. */

#include <mx/mx.h>

int main(int argc, char **argv) {
  mx_printf("hello from maxsdk: %d argument(s), the first %s\n", argc, argc > 0 ? argv[0] : "-");

  /* time something: the cycle counter counts EE cycles and wraps after
     about 14.6 seconds */
  u32 t0 = mx_count();
  float sum = 0;
  for (int i = 1; i <= 1000; i++)
    sum += 1.0f / (float)i;
  u32 cycles = mx_count() - t0;
  mx_printf("the sum of 1/i for i = 1..1000 is %f: %u cycles, %u ns\n", sum, cycles,
            cycles * 1000 / (MX_EE_HZ / 1000000));

  /* the heap: the memory between the program and the stack */
  mx_printf("heap %p .. %p\n", mx_heap_start(), mx_heap_end());

  MX_TEST_END(0);
  return 0;
}
