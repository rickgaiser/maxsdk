/* The __atomic_* calls the compiler emits for C11/C++ atomics: the R5900
   has no LL/SC, so clang lowers atomic operations to these calls. The EE
   has one core: interrupts off around the operation makes it atomic. */

#include <mx/cpu.h>

#define OPS(N, T)                                                                          \
  T __atomic_load_##N(const volatile void *p, int m) {                                    \
    (void)m;                                                                               \
    int was = mx_di();                                                                     \
    T v = *(const volatile T *)p;                                                          \
    mx_irq_restore(was);                                                                   \
    return v;                                                                              \
  }                                                                                        \
  void __atomic_store_##N(volatile void *p, T v, int m) {                                 \
    (void)m;                                                                               \
    int was = mx_di();                                                                     \
    *(volatile T *)p = v;                                                                  \
    mx_irq_restore(was);                                                                   \
  }                                                                                        \
  T __atomic_exchange_##N(volatile void *p, T v, int m) {                                 \
    (void)m;                                                                               \
    int was = mx_di();                                                                     \
    T o = *(volatile T *)p;                                                                \
    *(volatile T *)p = v;                                                                  \
    mx_irq_restore(was);                                                                   \
    return o;                                                                              \
  }                                                                                        \
  _Bool __atomic_compare_exchange_##N(volatile void *p, void *expected, T desired, int s,  \
                                      int f) {                                             \
    (void)s, (void)f;                                                                      \
    int was = mx_di();                                                                     \
    T o = *(volatile T *)p;                                                                \
    _Bool ok = o == *(T *)expected;                                                        \
    if (ok)                                                                                \
      *(volatile T *)p = desired;                                                          \
    else                                                                                   \
      *(T *)expected = o;                                                                  \
    mx_irq_restore(was);                                                                   \
    return ok;                                                                             \
  }                                                                                        \
  T __atomic_fetch_add_##N(volatile void *p, T v, int m) {                                \
    (void)m;                                                                               \
    int was = mx_di();                                                                     \
    T o = *(volatile T *)p;                                                                \
    *(volatile T *)p = (T)(o + v);                                                         \
    mx_irq_restore(was);                                                                   \
    return o;                                                                              \
  }                                                                                        \
  T __atomic_fetch_sub_##N(volatile void *p, T v, int m) {                                \
    (void)m;                                                                               \
    int was = mx_di();                                                                     \
    T o = *(volatile T *)p;                                                                \
    *(volatile T *)p = (T)(o - v);                                                         \
    mx_irq_restore(was);                                                                   \
    return o;                                                                              \
  }

OPS(1, unsigned char)
OPS(2, unsigned short)
OPS(4, unsigned int)
OPS(8, unsigned long long)
