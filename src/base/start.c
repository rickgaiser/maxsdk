/* C side of the startup: heap, cache, arguments, debug output, main, exit. */

#include <mx/bios.h>
#include <mx/cpu.h>
#include <mx/debug.h>
#include <mx/mx.h>

#define MX_MAX_ARGS 16

/* Filled in by the kernel in SetupThread (when started by ExecPS2). */
struct mx_args {
  s32 argc;
  char *argv[MX_MAX_ARGS];
  char payload[256];
};

/* What ps2link passes in a0 to __start. */
struct mx_loader_args {
  s32 pid;
  struct mx_args args;
};

struct mx_args mx__kernel_args;
struct mx_loader_args *mx__loader_args;

extern char _end[];
extern void (*__init_array_start[])(void);
extern void (*__init_array_end[])(void);

int main(int argc, char **argv);
void mx__libc_exit(int status) __attribute__((weak, noreturn));

static void *heap_end;

void *mx_heap_end(void) { return heap_end; }
void *mx_heap_start(void) { return _end; }

void mx_exit(int status) {
  mx_debug_flush();
  mx_bios_exit(status);
}

void mx__main(void) __attribute__((noreturn));
void mx__main(void) {
  heap_end = mx_bios_setup_heap(_end, -1);
  mx_bios_flush_cache(0);

  struct mx_args *a = &mx__kernel_args;
  if (a->argc == 0 && mx__loader_args && mx__loader_args->args.argc != 0)
    a = &mx__loader_args->args;

  mx_debug_init();
  mx_ei();

  for (void (**f)(void) = __init_array_start; f < __init_array_end; f++)
    (*f)();

  int status = main(a->argc, a->argv);
  if (mx__libc_exit)
    mx__libc_exit(status); /* libmxc: atexit handlers, static destructors */
  mx_exit(status);
}
