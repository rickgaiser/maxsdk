/* The BIOS of the PS2 the tests run on (docs/README.md "Hardware used"):
   rom0:ROMVER (VVVVRTYYYYMMDD: version, region, type, date) and the machine
   type, read through the IOP file server. Video modes beyond NTSC/PAL need
   a newer kernel; GetGsDxDyOffset only exists after 2001-06-08 (gsKit). */

#include <mx/sif.h>
#include "../tests/ps2/t.h"

int main(void) {
  static char v[32] MX_ALIGNED(64);
  int fd = mx_fio_open("rom0:ROMVER", MX_FIO_RDONLY);
  CHECK(fd >= 0);
  if (fd >= 0) {
    int n = mx_fio_lseek(fd, 0, MX_FIO_SEEK_END);
    mx_fio_lseek(fd, 0, MX_FIO_SEEK_SET);
    if (n > 31)
      n = 31;
    CHECK_EQ(mx_fio_read(fd, v, (u32)(n > 0 ? n : 0)), n);
    mx_fio_close(fd);
    for (int i = 0; i < n; i++)
      if (v[i] < ' ')
        v[i] = 0;
    mx_printf("bios: ROMVER %s (%d bytes)\n", v, n);
  }
  mx_printf("bios: machine type %d\n", mx_bios_machine_type());
  return t_done("bios_info");
}
