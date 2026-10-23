#ifndef MX_SIF_H
#define MX_SIF_H

/* Minimal EE-side SIF command/RPC client. It only talks to servers that are
   already running on the IOP (ps2link's file server, for tty output). There
   is no IOP code in maxsdk. */

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
/* Call function fno of a bound server. send/recv must be 16-byte aligned;
   recv is written by DMA, so it should be an uncached pointer. Blocks. */
int mx_sif_call(struct mx_sif_client *c, int fno, const void *send, u32 ssize, void *recv,
                u32 rsize);

/* Write to file descriptor fd of the IOP file server (1 = tty). */
int mx_tty_write(const char *s, u32 n);

#endif
