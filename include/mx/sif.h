#ifndef MX_SIF_H
#define MX_SIF_H

/* Minimal EE-side SIF command/RPC client. It talks to servers running on the
   IOP: ps2link's file server (tty output), LOADFILE (modules from rom0:) and
   the pad driver. There is no IOP code in maxsdk. */

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

#endif
