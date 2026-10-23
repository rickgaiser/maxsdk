#include <mx/cpu.h>
#include <mx/debug.h>
#include <mx/hw/regs.h>
#include <mx/sif.h>

/* The tty reaches ps2client on the PS2 and the IOP console on PCSX2. The
   serial port is the fallback when the IOP does not answer. */
static int sinks = MX_SINK_TTY;
static int hold;
static char hold_buf[16384];
static u32 hold_len;

static void sio_write(const char *s, u32 n) {
  for (u32 i = 0; i < n; i++) {
    if (s[i] == '\n') {
      while ((MX_R32(MX_SIO_ISR) & 0xF000) == 0x8000)
        ;
      *(volatile u8 *)MX_SIO_TXFIFO = '\r';
    }
    while ((MX_R32(MX_SIO_ISR) & 0xF000) == 0x8000)
      ;
    *(volatile u8 *)MX_SIO_TXFIFO = (u8)s[i];
  }
}

static void send(const char *s, u32 n) {
  if (sinks & MX_SINK_SIO)
    sio_write(s, n);
  if ((sinks & MX_SINK_TTY) && mx_tty_write(s, n) < 0) {
    sinks = (sinks & ~MX_SINK_TTY) | MX_SINK_SIO;
    sio_write(s, n);
  }
}

void mx_debug_init(void) {}

void mx_debug_sinks(int s) { sinks = s; }
int mx_debug_get_sinks(void) { return sinks; }

void mx_debug_flush(void) {
  if (hold_len)
    send(hold_buf, hold_len);
  hold_len = 0;
}

void mx_debug_hold(int h) {
  hold = h;
  if (!h)
    mx_debug_flush();
}

void mx_debug_write(const char *s, u32 n) {
  if (!hold) {
    send(s, n);
    return;
  }
  if (hold_len + n > sizeof hold_buf)
    mx_debug_flush();
  if (n > sizeof hold_buf) {
    send(s, n);
    return;
  }
  for (u32 i = 0; i < n; i++)
    hold_buf[hold_len++] = s[i];
}

void mx_exit(int status) __attribute__((noreturn));

/* MX_E_CHECK (<mx/emit.h>) in MX_DEBUG builds */
void mx__emit_overflow(const char *file, int line) {
  mx_debug_hold(0);
  mx_printf("emit: buffer overflow at %s:%d\n", file, line);
  mx_exit(99);
}
