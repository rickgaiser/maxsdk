#ifndef MX_SIF_H
#define MX_SIF_H

/* Minimal EE-side SIF command/RPC client. It talks to servers running on the
   IOP: ps2link's file server (tty output), LOADFILE (modules from rom0:) and
   the pad driver. There is no IOP code in maxsdk. IOP memory and the patches
   of its ROM modules: <mx/iop.h>. */

#include <mx/types.h>

struct mx_sif_client {
  volatile int done;
  u32 server; /* IOP server data, 0 until bound */
  u32 buf;    /* IOP receive buffer of the server */
  u32 cbuf;
  void *pkt; /* our packet of the request in flight */
};

/* Returns 0 on success, < 0 if the IOP side does not answer. */
int mx_sif_init(void);
int mx_sif_ready(void);
/* Bind to RPC server sid. Blocks (polls) until the IOP answers. */
int mx_sif_bind(struct mx_sif_client *c, u32 sid);
/* The same, giving up after n tries (0 if the server is not registered). */
int mx_sif_bind_tries(struct mx_sif_client *c, u32 sid, int n);
/* Call function fno of a bound server. send/recv must be 16-byte aligned;
   recv is written by DMA, so it should be an uncached pointer. Blocks. */
int mx_sif_call(struct mx_sif_client *c, int fno, const void *send, u32 ssize, void *recv,
                u32 rsize);

/* An EE RPC server for an IOP module that calls back (e.g. audsrv): binds
   to sid are answered, and each call runs fn(fno, arg) in the SIF interrupt
   handler and is then answered. The data a call sends (up to
   MX_SIF_SERVER_BUF bytes) is not kept; none goes back. Up to 4 servers.
   Returns 0, < 0 when there is no room. */
#define MX_SIF_SERVER_BUF 64
typedef void (*mx_sif_server_fn)(int fno, void *arg);
int mx_sif_serve(u32 sid, mx_sif_server_fn fn, void *arg);

/* Load and start an IOP module (LOADFILE), e.g. "rom0:PADMAN". Returns its
   module id, < 0 on failure. */
int mx_iop_load_module(const char *path);

/* Write to file descriptor fd of the IOP file server (1 = tty). */
int mx_tty_write(const char *s, u32 n);

/* Files of the IOP file server, e.g. "host:shot.tga" (ps2link: the directory
   of ps2client; PCSX2: its host root). Return < 0 on failure. */
#define MX_FIO_RDONLY 0x0001
#define MX_FIO_WRONLY 0x0002
#define MX_FIO_CREAT 0x0200
#define MX_FIO_TRUNC 0x0400
#define MX_FIO_SEEK_SET 0
#define MX_FIO_SEEK_CUR 1
#define MX_FIO_SEEK_END 2
int mx_fio_open(const char *path, int flags);
int mx_fio_write(int fd, const void *data, u32 n);
/* Read up to n bytes into data (any alignment, room for n bytes); returns the
   bytes read (fewer at the end of the file, and then the bytes of data after
   them may have changed). */
int mx_fio_read(int fd, void *data, u32 n);
/* Returns the new position: mx_fio_lseek(fd, 0, MX_FIO_SEEK_END) is the size. */
int mx_fio_lseek(int fd, s32 offset, int whence);
int mx_fio_close(int fd);
/* Directories: mx_fio_dread puts the next entry's name in name (room bytes)
   and its size in *size (if size); returns > 0, 0 after the last entry. */
int mx_fio_dopen(const char *path);
int mx_fio_dread(int fd, char *name, u32 room, u32 *size);
int mx_fio_dclose(int fd);
/* Remove a file, make a directory (mode: not used by the ROM's file server),
   remove an empty one. */
int mx_fio_remove(const char *path);
int mx_fio_mkdir(const char *path, int mode);
int mx_fio_rmdir(const char *path);

#endif
