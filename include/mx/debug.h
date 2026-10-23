#ifndef MX_DEBUG_H
#define MX_DEBUG_H

/* Debug output. Text goes to the enabled sinks:
   - MX_SINK_SIO: the EE serial port (PCSX2 shows it on its EE console);
   - MX_SINK_TTY: the IOP tty through the file server (ps2client on the PS2).
   With mx_debug_hold(1), text is only collected and sent by mx_debug_flush(),
   so timed code is not disturbed by SIF traffic. */

#include <mx/types.h>
#include <stdarg.h>

#define MX_SINK_SIO 1
#define MX_SINK_TTY 2

void mx_debug_init(void);
void mx_debug_sinks(int sinks);
int mx_debug_get_sinks(void);
void mx_debug_hold(int hold);
void mx_debug_flush(void);
void mx_debug_write(const char *s, u32 n);

/* Report CPU exceptions (address and bus errors, TLB misses, reserved
   instructions, ...) on the debug output with the registers, then exit with
   status 255. The startup installs it before main. */
void mx_exception_init(void);

int mx_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int mx_vprintf(const char *fmt, va_list ap);
int mx_snprintf(char *buf, u32 size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int mx_vsnprintf(char *buf, u32 size, const char *fmt, va_list ap);

#endif
