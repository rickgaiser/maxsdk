/* The pad driver: rom0:SIO2MAN and rom0:PADMAN load (or are there), port 0
   opens, and a pad on it is read within 2 s (its records come every frame).
   The buttons held at the end are printed. */

#include "t.h"
#include <mx/pad.h>

int main(void) {
  CHECK(mx_pad_init() == 0);
  u32 t0 = mx_count();
  while (!mx_pad_connected(0) && mx_count() - t0 < 2 * MX_EE_HZ)
    ;
  CHECK(mx_pad_connected(0));
  mx_printf("pad: connected after %u ms, buttons %04x\n", (mx_count() - t0) / (MX_EE_HZ / 1000),
            mx_pad_buttons(0));
  return t_done("pad");
}
