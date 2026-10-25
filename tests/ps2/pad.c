/* The pad driver: rom0:SIO2MAN and rom0:PADMAN load (or are there), port 0
   opens, and a pad on it is read within 2 s (its records come every frame).
   The buttons held at the end are printed. A DualShock is then in analog mode
   within 2 s more (mx_pad_sticks asks for it); a digital pad stays digital. */

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
  u8 st[4];
  int analog = 0;
  t0 = mx_count();
  while (!(analog = mx_pad_sticks(0, st)) && mx_count() - t0 < 2 * MX_EE_HZ)
    ;
  CHECK(mx_pad_connected(0));
  mx_printf("pad: %s, sticks %u %u %u %u\n", analog ? "analog" : "digital", st[0], st[1], st[2],
            st[3]);
  return t_done("pad");
}
