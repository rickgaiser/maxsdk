/* The exception report: crash three calls deep, so the report's trace and
   tools/symbolize.py can be checked. Ends with status 255 from the handler.
     crash.elf          break instruction (breakpoint)
     crash.elf align    misaligned load (address error; PCSX2 does not raise
                        it, so only on the PS2) */

#include <mx/mx.h>
#include <mx/string.h>

static volatile float scale = 1.5f;

__attribute__((noinline)) static int level3(const char *p, int align) {
  if (!align)
    __builtin_trap();
  return *(volatile const int *)(p + 1);
}

__attribute__((noinline)) static int level2(const char *p, int align) {
  return level3(p, align) + (int)(scale * 2.0f);
}

__attribute__((noinline)) static int level1(const char *p, int align) {
  return level2(p, align) + 1;
}

int main(int argc, char **argv) {
  static int word[2] = {1, 2};
  int align = argc > 1 && strcmp(argv[1], "align") == 0;
  mx_printf("crash: %s, expect the exception report and status 255\n",
            align ? "misaligned load" : "break");
  int r = level1((const char *)word, align);
  mx_printf("crash: no exception (%d)\n", r);
  MX_TEST_END(1);
  return 1;
}
