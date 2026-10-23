// The C++ runtime that maxsdk programs need without libstdc++/libc++:
// operator new/delete on picolibc's malloc, pure virtual calls, guards for
// local statics (one EE thread runs C++ code at a time), and __dso_handle.
// No exceptions, no RTTI (built with -fno-exceptions -fno-rtti).

#include <stddef.h>
#include <stdlib.h>

extern "C" void mx_exit(int status) __attribute__((noreturn));
extern "C" int mx_printf(const char *fmt, ...);

namespace std {
enum class align_val_t : size_t {};
struct nothrow_t {};
} // namespace std

void *operator new(size_t n) {
  void *p = malloc(n ? n : 1);
  if (!p) {
    mx_printf("operator new: out of memory (%u bytes)\n", (unsigned)n);
    mx_exit(98);
  }
  return p;
}
void *operator new[](size_t n) { return operator new(n); }
void *operator new(size_t n, const std::nothrow_t &) noexcept { return malloc(n ? n : 1); }
void *operator new[](size_t n, const std::nothrow_t &) noexcept { return malloc(n ? n : 1); }
void operator delete(void *p) noexcept { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete(void *p, size_t) noexcept { free(p); }
void operator delete[](void *p, size_t) noexcept { free(p); }

extern "C" {
void *__dso_handle = &__dso_handle;

void __cxa_pure_virtual(void) {
  mx_printf("pure virtual function called\n");
  mx_exit(97);
}

// The guard's first byte is set once the static is constructed.
int __cxa_guard_acquire(long long *g) { return *(char *)g == 0; }
void __cxa_guard_release(long long *g) { *(char *)g = 1; }
void __cxa_guard_abort(long long *g) { (void)g; }
}
