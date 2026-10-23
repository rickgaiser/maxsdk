#ifndef MX_TYPES_H
#define MX_TYPES_H

#include <stddef.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef unsigned int u32; /* not uint32_t: that is long on the EE gcc */
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int s32;
typedef int64_t s64;
/* A quadword. Built with mx_qw() from two 64-bit halves. */
typedef unsigned int u128 __attribute__((mode(TI)));

static inline u128 mx_qw(u64 lo, u64 hi) { return (u128)hi << 64 | lo; }
static inline u64 mx_qw_lo(u128 q) { return (u64)q; }
static inline u64 mx_qw_hi(u128 q) { return (u64)(q >> 64); }

#define MX_ALIGNED(n) __attribute__((aligned(n)))
#define MX_INLINE static inline __attribute__((always_inline))
#define MX_UNUSED __attribute__((unused))

#endif
