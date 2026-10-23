/* Compiler helper functions. The R5900 has no 64-bit divide, so 64-bit
   division is a library call. */

#include <mx/types.h>

static u64 udivmod(u64 n, u64 d, u64 *rem) {
  if ((n >> 32) == 0 && (d >> 32) == 0) {
    u32 q = (u32)n / (u32)d;
    if (rem)
      *rem = (u32)n - q * (u32)d;
    return q;
  }
  u64 q = 0, r = 0;
  for (int i = 63; i >= 0; i--) {
    r = r << 1 | ((n >> i) & 1);
    if (r >= d) {
      r -= d;
      q |= 1ull << i;
    }
  }
  if (rem)
    *rem = r;
  return q;
}

u64 __udivdi3(u64 n, u64 d) { return udivmod(n, d, 0); }

u64 __umoddi3(u64 n, u64 d) {
  u64 r;
  udivmod(n, d, &r);
  return r;
}

s64 __divdi3(s64 n, s64 d) {
  int neg = (n < 0) != (d < 0);
  u64 q = udivmod(n < 0 ? -(u64)n : (u64)n, d < 0 ? -(u64)d : (u64)d, 0);
  return neg ? -(s64)q : (s64)q;
}

s64 __moddi3(s64 n, s64 d) {
  u64 r;
  udivmod(n < 0 ? -(u64)n : (u64)n, d < 0 ? -(u64)d : (u64)d, &r);
  return n < 0 ? -(s64)r : (s64)r;
}

/* float -> double (printf promotes float arguments). The R5900 FPU has no
   double precision, so the compiler calls this. Bit manipulation only. */
double __extendsfdf2(float f) {
  union {
    float f;
    u32 u;
  } a = {f};
  union {
    u64 u;
    double d;
  } r;
  u64 sign = (u64)(a.u >> 31) << 63;
  u32 exp = (a.u >> 23) & 0xFF;
  u64 man = a.u & 0x7FFFFF;
  if (exp == 0xFF) {
    r.u = sign | 0x7FFull << 52 | man << 29;
  } else if (exp == 0) {
    if (!man) {
      r.u = sign;
    } else { /* denormal: normalize */
      int e = -126;
      while (!(man & 0x800000)) {
        man <<= 1;
        e--;
      }
      r.u = sign | (u64)(e + 1023) << 52 | (man & 0x7FFFFF) << 29;
    }
  } else {
    r.u = sign | (u64)(exp - 127 + 1023) << 52 | man << 29;
  }
  return r.d;
}
