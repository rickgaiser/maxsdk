/* maxsdk compatibility (D10a): ps2sdk's tamtypes.h types. */
#ifndef MX_COMPAT_TAMTYPES_H
#define MX_COMPAT_TAMTYPES_H
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef unsigned int u32;
typedef uint64_t u64;
typedef unsigned int u128 __attribute__((mode(TI)));
typedef int8_t s8;
typedef int16_t s16;
typedef int s32;
typedef int64_t s64;
typedef int s128 __attribute__((mode(TI)));
#endif
