/* Files of the IOP file server (mx_fio_*): write host:fio_test.bin, then its
   size by lseek, and read it back in pieces of odd sizes into a buffer at an
   odd address (the IOP sends the 16-byte aligned middle by DMA and the parts
   around it in a record), over the 64 KiB of one READ; the bytes around the
   buffer (but those of the last request after the end of the file) must keep
   their values. Then a directory made, a file in it, both removed. */

#include <mx/sif.h>
#include "t.h"

#define SIZE 70001
static u8 data[SIZE] MX_ALIGNED(64);
static u8 back[SIZE + 128] MX_ALIGNED(64);

int main(void) {
  for (u32 i = 0; i < SIZE; i++)
    data[i] = (u8)(i * 7 + (i >> 8));
  int fd = mx_fio_open("host:fio_test.bin", MX_FIO_WRONLY | MX_FIO_CREAT | MX_FIO_TRUNC);
  CHECK(fd >= 0);
  if (fd < 0)
    return t_done("fio");
  CHECK_EQ(mx_fio_write(fd, data, SIZE), 0);
  CHECK(mx_fio_close(fd) >= 0);

  fd = mx_fio_open("host:fio_test.bin", MX_FIO_RDONLY);
  CHECK(fd >= 0);
  if (fd < 0)
    return t_done("fio");
  CHECK_EQ(mx_fio_lseek(fd, 0, MX_FIO_SEEK_END), SIZE);
  CHECK_EQ(mx_fio_lseek(fd, 0, MX_FIO_SEEK_SET), 0);
  for (u32 i = 0; i < sizeof back; i++)
    back[i] = 0xA5;
  /* from back + 5: every alignment of the start and the end of a piece */
  static const u32 pieces[] = {1, 15, 16, 17, 3, 64, 1000, 65536 + 7};
  u8 *d = back + 5;
  u32 at = 0;
  for (u32 p = 0; p < sizeof pieces / sizeof pieces[0]; p++) {
    int got = mx_fio_read(fd, d + at, pieces[p]);
    CHECK_EQ(got, pieces[p]);
    at += pieces[p];
  }
  /* the rest, asking for more than there is: the 64 bytes after it are in
     the request, the guard after them is not */
  int got = mx_fio_read(fd, d + at, SIZE - at + 64);
  CHECK_EQ(got, SIZE - at);
  CHECK_EQ(mx_fio_read(fd, d, 16), 0);
  /* back from the end and from the current position (ps2client would leave
     its file past the end, but for mx_fio_lseek) */
  static u8 tail[64] MX_ALIGNED(64);
  CHECK_EQ(mx_fio_lseek(fd, -10, MX_FIO_SEEK_END), SIZE - 10);
  CHECK_EQ(mx_fio_read(fd, tail, 64), 10);
  CHECK_EQ(tail[0], data[SIZE - 10]);
  CHECK_EQ(mx_fio_lseek(fd, -20, MX_FIO_SEEK_CUR), SIZE - 20);
  CHECK_EQ(mx_fio_read(fd, tail, 4), 4);
  CHECK_EQ(tail[3], data[SIZE - 17]);
  CHECK_EQ(mx_fio_lseek(fd, 2, MX_FIO_SEEK_CUR), SIZE - 14);
  CHECK_EQ(mx_fio_read(fd, tail, 1), 1);
  CHECK_EQ(tail[0], data[SIZE - 14]);
  /* ps2link answers a close with the descriptor, PCSX2 with 0 */
  CHECK(mx_fio_close(fd) >= 0);
  u32 bad = 0;
  for (u32 i = 0; i < SIZE; i++)
    if (d[i] != data[i] && bad++ < 4)
      mx_printf("fio: byte %u = %02x, expected %02x\n", i, d[i], data[i]);
  for (u32 i = 0; i < sizeof back; i++)
    if ((i < 5 || i >= 5 + SIZE + 64) && back[i] != 0xA5 && bad++ < 12)
      mx_printf("fio: guard byte %d after the data = %02x\n", (s32)i - 5 - SIZE, back[i]);
  CHECK_EQ(bad, 0);
  mx_printf("fio: %u bytes written and read back\n", SIZE);

  /* mkdir, remove, rmdir (whether the directory was there before or not) */
  mx_fio_remove("host:fio_test_dir/f.bin");
  mx_fio_rmdir("host:fio_test_dir");
  CHECK(mx_fio_mkdir("host:fio_test_dir", 0777) >= 0);
  int dd = mx_fio_dopen("host:fio_test_dir");
  CHECK(dd >= 0);
  if (dd >= 0)
    mx_fio_dclose(dd);
  fd = mx_fio_open("host:fio_test_dir/f.bin", MX_FIO_WRONLY | MX_FIO_CREAT | MX_FIO_TRUNC);
  CHECK(fd >= 0);
  if (fd >= 0) {
    CHECK_EQ(mx_fio_write(fd, data, 16), 0);
    mx_fio_close(fd);
  }
  /* PCSX2 removes the file but answers with an error (docs/pcsx2.md): the
     file is gone either way */
  mx_fio_remove("host:fio_test_dir/f.bin");
  fd = mx_fio_open("host:fio_test_dir/f.bin", MX_FIO_RDONLY);
  CHECK(fd < 0);
  if (fd >= 0)
    mx_fio_close(fd);
  CHECK(mx_fio_rmdir("host:fio_test_dir") >= 0);
  dd = mx_fio_dopen("host:fio_test_dir");
  CHECK(dd < 0);
  if (dd >= 0)
    mx_fio_dclose(dd);
  return t_done("fio");
}
