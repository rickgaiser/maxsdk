/* Built with -fno-builtin, so the loops are not turned back into calls to
   these functions. */
#include <mx/string.h>

/* Weak: with a C library linked (libmxc + picolibc), its versions win. */
#define MX_WEAK __attribute__((weak))

MX_WEAK void *memcpy(void *d, const void *s, size_t n) {
  u8 *dp = d;
  const u8 *sp = s;
  if ((((uintptr_t)dp | (uintptr_t)sp) & 15) == 0) {
    for (; n >= 16; n -= 16, dp += 16, sp += 16)
      *(u128 *)dp = *(const u128 *)sp;
  } else if ((((uintptr_t)dp | (uintptr_t)sp) & 3) == 0) {
    for (; n >= 4; n -= 4, dp += 4, sp += 4)
      *(u32 *)dp = *(const u32 *)sp;
  }
  while (n--)
    *dp++ = *sp++;
  return d;
}

MX_WEAK void *memmove(void *d, const void *s, size_t n) {
  u8 *dp = d;
  const u8 *sp = s;
  if (dp <= sp || dp >= sp + n)
    return memcpy(d, s, n);
  while (n--)
    dp[n] = sp[n];
  return d;
}

MX_WEAK void *memset(void *d, int c, size_t n) {
  u8 *dp = d;
  if (((uintptr_t)dp & 15) == 0 && n >= 16) {
    u32 w = (u8)c * 0x01010101u;
    u64 b = (u64)w << 32 | w;
    u8 *end = dp + (n & ~(size_t)15);
    n &= 15;
    /* one 128-bit store per quadword */
    __asm__ volatile(".set push\n\t.set noreorder\n\t"
                     "pcpyld $8, %1, %1\n"
                     "1:\tsq $8, 0(%0)\n\t"
                     "addiu %0, %0, 16\n\t"
                     "bne %0, %2, 1b\n\t"
                     "nop\n\t"
                     ".set pop"
                     : "+r"(dp)
                     : "r"(b), "r"(end)
                     : "$8", "memory");
  }
  while (n--)
    *(volatile u8 *)dp++ = (u8)c;
  return d;
}

MX_WEAK int memcmp(const void *a, const void *b, size_t n) {
  const u8 *x = a, *y = b;
  for (; n; n--, x++, y++)
    if (*x != *y)
      return *x - *y;
  return 0;
}

MX_WEAK size_t strlen(const char *s) {
  const volatile char *p = s;
  while (*p)
    p++;
  return (size_t)(p - s);
}

MX_WEAK int strcmp(const char *a, const char *b) {
  while (*a && *a == *b)
    a++, b++;
  return (u8)*a - (u8)*b;
}

MX_WEAK int strncmp(const char *a, const char *b, size_t n) {
  for (; n; n--, a++, b++) {
    if (*a != *b)
      return (u8)*a - (u8)*b;
    if (!*a)
      return 0;
  }
  return 0;
}

MX_WEAK char *strcpy(char *d, const char *s) {
  char *r = d;
  while ((*d++ = *s++))
    ;
  return r;
}
