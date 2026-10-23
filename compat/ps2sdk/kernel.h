/* maxsdk compatibility (D10a): the parts of ps2sdk's kernel.h bench uses. */
#ifndef MX_COMPAT_KERNEL_H
#define MX_COMPAT_KERNEL_H
#include <tamtypes.h>
#include <mx/cpu.h>
#define DI() ((void)mx_di())
#define EI() mx_ei()
#endif
