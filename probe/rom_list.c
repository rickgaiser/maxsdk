/* The IOP modules of the BIOS: rom0:ROMDIR (the ROM's file table) and a few
   files of rom0: copied to the host, for reading there. ROMDRV has no
   directory reads (dopen works, dread gives no entries). */

#include <mx/sif.h>
#include "../tests/ps2/t.h"

static u8 buf[256 * 1024] MX_ALIGNED(64);

static void copy(const char *src, const char *dst) {
  int f = mx_fio_open(src, MX_FIO_RDONLY);
  if (f < 0) {
    mx_printf("rom: %s: %d\n", src, f);
    return;
  }
  int n = mx_fio_lseek(f, 0, MX_FIO_SEEK_END);
  mx_fio_lseek(f, 0, MX_FIO_SEEK_SET);
  CHECK(n > 0 && n <= (int)sizeof(buf));
  CHECK_EQ(mx_fio_read(f, buf, (u32)n), n);
  mx_fio_close(f);
  int o = mx_fio_open(dst, MX_FIO_WRONLY | MX_FIO_CREAT | MX_FIO_TRUNC);
  CHECK(o >= 0);
  for (int i = 0; i < n;) {
    int k = n - i > 1024 ? 1024 : n - i;
    CHECK_EQ(mx_fio_write(o, buf + i, (u32)k), k);
    i += k;
  }
  mx_fio_close(o);
  mx_printf("rom: %s -> %s, %d bytes\n", src, dst, n);
}

int main(void) {
  copy("rom0:ROMDIR", "host:ROMDIR.bin");
  copy("rom0:OSDSND", "host:OSDSND.bin");
  copy("rom0:LIBSD", "host:LIBSD.bin");
  copy("rom1:ROMDIR", "host:ROMDIR1.bin");
  return t_done("rom_list");
}
