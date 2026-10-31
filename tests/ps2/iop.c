/* The IOP from the EE (mx_iop_*): the modules LOADCORE lists, MODLOAD's export
   library, and the patches of the ROM modules (ps2sdk's sbv). Each patch is
   applied twice: the second time it must find itself (as LMB and prefix
   check do under ps2link, which applies them). rom0:SIO2MAN and rom0:PADMAN
   are read through the file server and loaded from EE memory
   (LoadModuleBuffer), then the pad driver opens and host: is listed again and again while PADMAN's
   records come every frame (the FILEIO dread race). */

#include <mx/iop.h>
#include <mx/pad.h>
#include <mx/sif.h>
#include "t.h"

static u8 irx[64 * 1024] MX_ALIGNED(64);

static void list_modules(void) {
  struct mx_iop_module m;
  u32 a = 0x800;
  for (int k = 0; a && k < 64; k++, a = m.next) {
    char name[32] = {0};
    if (mx_iop_read(a, &m, sizeof m) < 0)
      break;
    if (m.name)
      mx_iop_read(m.name, name, sizeof name - 1);
    mx_printf("iop: module %2u %-24s %x.%02x text %06x+%x\n", m.id, name, m.version >> 8,
              m.version & 0xff, m.text_start, m.text_size);
  }
}

static void load_buffer(const char *path) {
  int fd = mx_fio_open(path, MX_FIO_RDONLY);
  CHECK(fd >= 0);
  if (fd < 0)
    return;
  int size = mx_fio_lseek(fd, 0, MX_FIO_SEEK_END);
  CHECK(size > 0 && size <= (int)sizeof irx);
  mx_fio_lseek(fd, 0, MX_FIO_SEEK_SET);
  if (size > 0 && size <= (int)sizeof irx)
    CHECK_EQ(mx_fio_read(fd, irx, (u32)size), size);
  mx_fio_close(fd);
  int modres = -99;
  int id = mx_iop_load_module_buffer(irx, (u32)size, 0, 0, &modres);
  mx_printf("iop: %s from EE memory (%d bytes): id %d, start result %d\n", path, size, id, modres);
  CHECK(id >= 0);
}

int main(void) {
  CHECK(mx_sif_init() == 0);
  list_modules();

  u32 ex[16];
  int n = mx_iop_find_exports("modload", ex, 16);
  mx_printf("iop: modload has %d exports, 8 %x 10 %x 15 %x\n", n, ex[8], ex[10], ex[15]);
  CHECK(n >= 16);
  CHECK(mx_iop_find_exports("loadcore", 0, 0) > 0);
  CHECK(mx_iop_find_exports("nosuch", 0, 0) == 0);

  struct mx_iop_module m;
  CHECK(mx_iop_find_module("LoadModuleByEE", &m) > 0);
  int fileio = mx_iop_find_module("FILEIO_service", &m) ? m.version : 0;
  mx_printf("iop: FILEIO_service version %x\n", fileio);

  int r = mx_iop_patch_fileio();
  mx_printf("iop: patch fileio %d, again %d\n", r, mx_iop_patch_fileio());
  CHECK(r >= 0);
  CHECK_EQ(mx_iop_patch_fileio(), 1);
  if (fileio == 0x101) {
    u32 w;
    mx_iop_read(m.text_start + 0x9cc, &w, 4);
    CHECK(((w >> 26) == 3) && ((w & 0x3ffffff) << 2) - m.text_start >= m.text_size);
  }

  r = mx_iop_patch_lmb();
  mx_printf("iop: patch lmb %d, again %d\n", r, mx_iop_patch_lmb());
  CHECK(r >= 0);
  CHECK_EQ(mx_iop_patch_lmb(), 1);

  r = mx_iop_patch_prefix_check();
  mx_printf("iop: patch prefix check %d, again %d\n", r, mx_iop_patch_prefix_check());
  CHECK(r >= 0);
  CHECK_EQ(mx_iop_patch_prefix_check(), 1);
  u32 w[2];
  mx_iop_read(ex[15], w, 8);
  CHECK_EQ(w[0], 0x03e00008);
  CHECK_EQ(w[1], 0x00001021);

  load_buffer("rom0:SIO2MAN");
  load_buffer("rom0:PADMAN");

  CHECK(mx_pad_init() == 0);
  u32 t0 = mx_count();
  int lists = 0, first = -1, connected = 0;
  static char name[64];
  while (mx_count() - t0 < 5 * MX_EE_HZ) {
    int fd = mx_fio_dopen("host:");
    CHECK(fd >= 0);
    if (fd < 0)
      break;
    int k = 0;
    while (mx_fio_dread(fd, name, sizeof name, 0) > 0)
      k++;
    mx_fio_dclose(fd);
    if (first < 0)
      first = k;
    CHECK_EQ(k, first);
    if (k != first)
      break;
    lists++;
    connected += mx_pad_connected(0);
  }
  mx_printf("iop: host: listed %d times (%d entries), pad connected in %d\n", lists, first,
            connected);
  CHECK(lists > 0);
  return t_done("iop");
}
