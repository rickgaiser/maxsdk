/* maxsdk compatibility (D10a): the system time of ps2sdk's timer.h. */
#ifndef MX_COMPAT_TIMER_H
#define MX_COMPAT_TIMER_H
#include <tamtypes.h>
#define kBUSCLK (147456000)
#define kBUSCLKBY16 (kBUSCLK / 16)
#define kBUSCLKBY256 (kBUSCLK / 256)
/* bus clock ticks since start */
u64 GetTimerSystemTime(void);
#endif
