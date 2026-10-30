/* Minimal SIF command and RPC client, enough to write to the IOP file
   server's tty. The packet layouts and command IDs are the IOP SIF protocol
   (SIF CMD/RPC, as implemented by the IOP ROM modules sifcmd/sifman). */

#include <mx/bios.h>
#include <mx/bits.h>
#include <mx/cpu.h>
#include <mx/hw/formats.h>
#include <mx/hw/regs.h>
#include <mx/sif.h>
#include <mx/string.h>

/* System command IDs */
#define CMD_CHANGE_SADDR 0x80000000u
#define CMD_SET_SREG 0x80000001u
#define CMD_INIT_CMD 0x80000002u
#define CMD_RPC_END 0x80000008u
#define CMD_RPC_BIND 0x80000009u
#define CMD_RPC_CALL 0x8000000Au
#define CMD_RPC_RDATA 0x8000000Cu

/* SIF registers (sceSifGetReg/SetReg) */
#define REG_SUBADDR 2
#define REG_SMFLAG 4
#define SYSREG_SUBADDR 0x80000000u
#define SYSREG_RPCINIT 0x80000002u
#define SMFLAG_CMDINIT 0x20000

/* sceSifSetDma attributes */
#define DMA_INT_O 0x4
#define DMA_ERT 0x40

#define FILEIO_SID 0x80000001u
#define FILEIO_OPEN 0
#define FILEIO_CLOSE 1
#define FILEIO_READ 2
#define FILEIO_WRITE 3
#define FILEIO_LSEEK 4
#define FILEIO_DOPEN 9
#define FILEIO_DCLOSE 10
#define FILEIO_DREAD 11

#define RPC_PKT_SIZE 64
#define NPKT 8

struct cmd_hdr {
  u32 size; /* psize:8, dsize:24 */
  u32 dest;
  u32 cid;
  u32 opt;
};

struct sif_dma {
  u32 src, dest;
  s32 size, attr;
};

/* Received packets land here by DMA from the IOP. Only touched through the
   uncached alias. */
static u8 rx_buf[128] MX_ALIGNED(64);
/* Outgoing RPC packets, and replies to IOP requests (a small ring). */
static u8 pkt_pool[NPKT][RPC_PKT_SIZE] MX_ALIGNED(64);
static u8 reply_ring[NPKT][RPC_PKT_SIZE] MX_ALIGNED(64);
static volatile u32 pkt_busy;
static u32 reply_idx;
static u32 rpc_pid = 1;

static volatile u32 iop_buf;
static volatile s32 sregs[32];
static int sif_state; /* 0 = not tried, 1 = ready, -1 = no IOP */
static int sif0_handler_id MX_UNUSED;

static volatile u32 *uw(void *p) { return (volatile u32 *)MX_UNCACHED(p); }

static u32 send_cmd(int intr, u32 cid, void *pkt, u32 psize, const void *src, u32 dest,
                    u32 dsize) {
  volatile u32 *h = uw(pkt);
  struct sif_dma t[2];
  int n = 0;
  h[0] = (psize & 0xFF) | (dsize << 8);
  h[1] = dsize ? dest : 0;
  h[2] = cid;
  if (dsize) {
    t[n].src = (u32)(uintptr_t)src;
    t[n].dest = dest;
    t[n].size = (s32)dsize;
    t[n].attr = 0;
    n++;
  }
  t[n].src = (u32)(uintptr_t)MX_UNCACHED(pkt);
  t[n].dest = iop_buf;
  t[n].size = (s32)psize;
  t[n].attr = DMA_ERT | DMA_INT_O;
  n++;
  mx_sync();
  if (intr)
    return mx_bios_isif_set_dma(t, n);
  u32 id;
  while (!(id = mx_bios_sif_set_dma(t, n)))
    ;
  return id;
}

static void *rpc_alloc(void) {
  for (;;) {
    int was = mx_di();
    for (int i = 0; i < NPKT; i++) {
      if (!(pkt_busy & (1u << i))) {
        pkt_busy |= 1u << i;
        mx_irq_restore(was);
        volatile u32 *p = uw(pkt_pool[i]);
        for (int k = 0; k < RPC_PKT_SIZE / 4; k++)
          p[k] = 0;
        u32 pid = rpc_pid++;
        p[4] = ((u32)i << 16) | 0x05; /* rec_id: index, flags */
        p[5] = (u32)(uintptr_t)MX_UNCACHED(pkt_pool[i]);
        p[6] = pid;
        return pkt_pool[i];
      }
    }
    mx_irq_restore(was);
  }
}

static void rpc_free(void *p) {
  u32 i = (u32)((u8 *)p - pkt_pool[0]) / RPC_PKT_SIZE;
  if (i < NPKT)
    pkt_busy &= ~(1u << i);
}

/* SIF0 (IOP -> EE) DMA end: one command packet is in rx_buf. */
static int sif0_handler(int ch, void *arg, void *addr) {
  (void)ch, (void)arg, (void)addr;
  volatile u32 *rx = uw(rx_buf);
  u32 psize = rx[0] & 0xFF;
  if (!psize)
    return 0;
  u32 pkt[28];
  u32 words = (psize + 3) / 4;
  if (words > 28)
    words = 28;
  for (u32 i = 0; i < words; i++)
    pkt[i] = rx[i];
  rx[0] = 0;
  mx_bios_isif_set_dchain();

  switch (pkt[2]) {
  case CMD_CHANGE_SADDR:
    iop_buf = pkt[4];
    break;
  case CMD_SET_SREG:
    if (pkt[4] < 32)
      sregs[pkt[4]] = (s32)pkt[5];
    break;
  case CMD_RPC_END: {
    struct mx_sif_client *c = (struct mx_sif_client *)(uintptr_t)pkt[7];
    if (pkt[8] == CMD_RPC_BIND) {
      c->server = pkt[9];
      c->buf = pkt[10];
      c->cbuf = pkt[11];
    }
    /* the reply's packet address is the IOP's; free ours from the client */
    rpc_free(c->pkt);
    c->done = 1;
    break;
  }
  case CMD_RPC_RDATA: {
    /* The IOP wants EE memory (e.g. the data of a write): send it back with
       an RPC_END. */
    u8 *r = reply_ring[reply_idx++ % NPKT];
    volatile u32 *rp = uw(r);
    rp[5] = pkt[5];
    rp[7] = pkt[7];
    rp[8] = CMD_RPC_RDATA;
    send_cmd(1, CMD_RPC_END, r, RPC_PKT_SIZE, (const void *)(uintptr_t)pkt[8], pkt[9],
             pkt[10]);
    break;
  }
  default:
    break;
  }
  return 0;
}

int mx_sif_ready(void) { return sif_state == 1; }

int mx_sif_init(void) {
  if (sif_state)
    return sif_state > 0 ? 0 : -1;
  sif_state = -1;

  /* Clear a pending SIF0 end, and start receiving if nobody did. */
  if (MX_R32(MX_D_STAT) & (1u << MX_DMAC_SIF0))
    MX_R32(MX_D_STAT) = 1u << MX_DMAC_SIF0;
  if (!(MX_R32(MX_DMA_SIF0_CHCR) & MX_BIT(DMA_CHCR_STR)))
    mx_bios_sif_set_dchain();

  sif0_handler_id = mx_bios_add_dmac_handler(MX_DMAC_SIF0, sif0_handler, 0, 0);
  mx_bios_enable_dmac(MX_DMAC_SIF0);

  static u8 init_pkt[32] MX_ALIGNED(64);
  iop_buf = (u32)mx_bios_sif_get_reg(SYSREG_SUBADDR);
  if (iop_buf) {
    /* The IOP side is already set up (by ps2link): tell it our receive
       buffer. */
    uw(init_pkt)[4] = (u32)(uintptr_t)MX_UNCACHED(rx_buf);
    send_cmd(0, CMD_CHANGE_SADDR, init_pkt, 32, 0, 0, 0);
  } else {
    u32 t0 = mx_count();
    while (!(mx_bios_sif_get_reg(REG_SMFLAG) & SMFLAG_CMDINIT))
      if (mx_count() - t0 > 300000000u)
        return -1;
    iop_buf = (u32)mx_bios_sif_get_reg(REG_SUBADDR);
    mx_bios_sif_set_reg(SYSREG_SUBADDR, (s32)iop_buf);
    uw(init_pkt)[3] = 0;
    uw(init_pkt)[4] = (u32)(uintptr_t)MX_UNCACHED(rx_buf);
    send_cmd(0, CMD_INIT_CMD, init_pkt, 32, 0, 0, 0);
  }

  if (!mx_bios_sif_get_reg(SYSREG_RPCINIT)) {
    static u8 rpc_init_pkt[16] MX_ALIGNED(64);
    uw(rpc_init_pkt)[3] = 1;
    send_cmd(0, CMD_INIT_CMD, rpc_init_pkt, 16, 0, 0, 0);
    u32 t0 = mx_count();
    while (!sregs[0])
      if (mx_count() - t0 > 300000000u)
        return -1;
    mx_bios_sif_set_reg(SYSREG_RPCINIT, 1);
  }
  sif_state = 1;
  return 0;
}

static int wait_done(struct mx_sif_client *c) {
  u32 t0 = mx_count();
  while (!c->done)
    if (mx_count() - t0 > 600000000u)
      return -1;
  return 0;
}

int mx_sif_bind(struct mx_sif_client *c, u32 sid) { return mx_sif_bind_tries(c, sid, 1000); }

int mx_sif_bind_tries(struct mx_sif_client *c, u32 sid, int n) {
  for (int tries = 0; tries < n; tries++) {
    u8 *p = rpc_alloc();
    volatile u32 *w = uw(p);
    c->pkt = p;
    c->done = 0;
    c->server = 0;
    w[7] = (u32)(uintptr_t)c;
    w[8] = sid;
    send_cmd(0, CMD_RPC_BIND, p, RPC_PKT_SIZE, 0, 0, 0);
    if (wait_done(c) < 0)
      return -1;
    if (c->server)
      return 0;
  }
  return -1;
}

int mx_sif_call(struct mx_sif_client *c, int fno, const void *send, u32 ssize, void *recv,
                u32 rsize) {
  u8 *p = rpc_alloc();
  volatile u32 *w = uw(p);
  c->pkt = p;
  c->done = 0;
  w[7] = (u32)(uintptr_t)c;
  w[8] = (u32)fno;
  w[9] = ssize;
  w[10] = (u32)(uintptr_t)recv;
  w[11] = rsize;
  w[12] = 1; /* rmode: send RPC_END when done */
  w[13] = c->server;
  if (!mx_is_spr(send) && ((uintptr_t)send & 0xF0000000u) != 0x20000000u)
    mx_dcache_writeback(send, ssize);
  send_cmd(0, CMD_RPC_CALL, p, RPC_PKT_SIZE, send, c->buf, ssize);
  return wait_done(c);
}

/* ---- IOP modules ---------------------------------------------------------- */

#define LOADFILE_SID 0x80000006

/* The argument of LOADFILE's function 0 (load a module): the result comes
   back in its first two words. */
struct lf_load_arg {
  s32 result; /* in: the length of args */
  s32 modres;
  char path[252];
  char args[252];
};

static struct mx_sif_client lf;
static struct lf_load_arg lf_arg MX_ALIGNED(64);
static s32 lf_result[4] MX_ALIGNED(64);

int mx_iop_load_module(const char *path) {
  if ((!mx_sif_ready() && mx_sif_init() < 0) || (!lf.server && mx_sif_bind(&lf, LOADFILE_SID) < 0))
    return -1;
  u32 i = 0;
  for (; path[i] && i < sizeof lf_arg.path - 1; i++)
    lf_arg.path[i] = path[i];
  lf_arg.path[i] = 0;
  lf_arg.result = 0;
  lf_arg.modres = 0;
  volatile s32 *r = MX_UNCACHED(lf_result);
  if (mx_sif_call(&lf, 0, &lf_arg, sizeof lf_arg, (void *)r, 8) < 0)
    return -1;
  return r[0];
}

/* ---- tty ---------------------------------------------------------------- */

struct fio_write_arg {
  s32 fd;
  u32 ptr;
  u32 size;
  u32 mis;
  u8 aligned[16];
};

static struct mx_sif_client fio;
static struct fio_write_arg wr_arg MX_ALIGNED(64);
static u32 wr_result[4] MX_ALIGNED(64);
static char wr_data[1024] MX_ALIGNED(64);

static int fio_bind(void) {
  if (!fio.server && (mx_sif_init() < 0 || mx_sif_bind(&fio, FILEIO_SID) < 0))
    return -1;
  return 0;
}

int mx_fio_write(int fd, const void *data, u32 n) {
  if (fio_bind() < 0)
    return -1;
  const char *s = data;
  while (n) {
    u32 len = n > sizeof wr_data ? sizeof wr_data : n;
    char *d = MX_UNCACHED(wr_data);
    for (u32 i = 0; i < len; i++)
      d[i] = s[i];
    struct fio_write_arg *a = MX_UNCACHED(&wr_arg);
    a->fd = fd;
    a->ptr = (u32)(uintptr_t)d;
    a->size = len;
    a->mis = 0;
    if (mx_sif_call(&fio, FILEIO_WRITE, a, sizeof *a, MX_UNCACHED(wr_result), 4) < 0)
      return -1;
    s += len;
    n -= len;
  }
  return 0;
}

int mx_tty_write(const char *s, u32 n) { return mx_fio_write(1, s, n); }

/* The open arguments of the file server: flags, then the path. */
static struct {
  s32 flags;
  char path[256];
} op_arg MX_ALIGNED(64);

int mx_fio_open(const char *path, int flags) {
  if (fio_bind() < 0)
    return -1;
  __typeof__(op_arg) *a = MX_UNCACHED(&op_arg);
  a->flags = flags;
  u32 i = 0;
  for (; path[i] && i < sizeof a->path - 1; i++)
    a->path[i] = path[i];
  a->path[i] = 0;
  volatile s32 *r = (volatile s32 *)MX_UNCACHED(wr_result);
  if (mx_sif_call(&fio, FILEIO_OPEN, a, sizeof *a, (void *)r, 4) < 0)
    return -1;
  return r[0];
}

/* READ: the IOP sends the 16-byte aligned middle of the data by SIF DMA to
   ptr, and the parts before and after it in the record at read_data (the EE
   copies them). The parts are those of the size asked for: after a short
   read (the end of the file) the DMA ends on the quadword after the last byte
   read, and the part after it is not data. */
struct fio_read_data {
  u32 size1, size2;
  u32 dest1, dest2;
  u8 buf1[16], buf2[16];
};
static struct {
  s32 fd;
  u32 ptr;
  s32 size;
  u32 read_data;
} rd_arg MX_ALIGNED(64);
static struct fio_read_data rd_data MX_ALIGNED(64);

/* One READ of at most n bytes; returns the bytes read. */
static int fio_read1(int fd, u8 *d, u32 n) {
  /* the IOP's DMA must not meet dirty lines, nor the EE stale ones after it */
  mx_dcache_writeback(d, n);
  mx_dcache_invalidate(d, n);
  volatile struct fio_read_data *rd = MX_UNCACHED(&rd_data);
  rd->size1 = rd->size2 = 0;
  __typeof__(rd_arg) *a = MX_UNCACHED(&rd_arg);
  a->fd = fd;
  a->ptr = (u32)(uintptr_t)d;
  a->size = (s32)n;
  a->read_data = (u32)(uintptr_t)&rd_data;
  volatile s32 *r = (volatile s32 *)MX_UNCACHED(wr_result);
  if (mx_sif_call(&fio, FILEIO_READ, a, sizeof *a, (void *)r, 4) < 0)
    return -1;
  int got = r[0];
  mx_dcache_invalidate(d, n);
  if (got > 0) {
    u8 *end = d + got, *d1 = (u8 *)(uintptr_t)rd->dest1, *d2 = (u8 *)(uintptr_t)rd->dest2;
    for (u32 i = 0; i < rd->size1 && i < 16 && d1 + i < end; i++)
      d1[i] = rd->buf1[i];
    if (d2 >= d && d2 + rd->size2 <= end)
      for (u32 i = 0; i < rd->size2 && i < 16; i++)
        d2[i] = rd->buf2[i];
  }
  return got;
}

int mx_fio_read(int fd, void *data, u32 n) {
  if (fio_bind() < 0)
    return -1;
  u8 *d = data;
  u32 total = 0;
  while (n) {
    u32 len = n > 0x10000 ? 0x10000 : n;
    int got = fio_read1(fd, d, len);
    if (got < 0)
      return total ? (int)total : got;
    total += (u32)got;
    if ((u32)got < len)
      break; /* the end of the file */
    d += len;
    n -= len;
  }
  return (int)total;
}

static int fio_lseek1(int fd, s32 offset, int whence) {
  static s32 ls_arg[4] MX_ALIGNED(64);
  volatile s32 *a = (volatile s32 *)MX_UNCACHED(ls_arg);
  a[0] = fd;
  a[1] = offset;
  a[2] = whence;
  volatile s32 *r = (volatile s32 *)MX_UNCACHED(wr_result);
  if (mx_sif_call(&fio, FILEIO_LSEEK, (void *)a, 16, (void *)r, 4) < 0)
    return -1;
  return r[0];
}

int mx_fio_lseek(int fd, s32 offset, int whence) {
  if (fio_bind() < 0)
    return -1;
  /* ps2client (ps2link's host) takes a negative offset from the current
     position or the end as a huge positive one: it answers with the right
     position, but its file is then past the end (docs/bios.md). From the
     position itself instead. */
  if (offset < 0 && whence != MX_FIO_SEEK_SET) {
    int base = fio_lseek1(fd, 0, whence);
    if (base < 0)
      return base;
    if (base + offset < 0)
      return -1;
    return fio_lseek1(fd, base + offset, MX_FIO_SEEK_SET);
  }
  return fio_lseek1(fd, offset, whence);
}

int mx_fio_close(int fd) {
  if (fio_bind() < 0)
    return -1;
  static s32 cl_arg[4] MX_ALIGNED(64);
  volatile s32 *a = (volatile s32 *)MX_UNCACHED(cl_arg);
  a[0] = fd;
  volatile s32 *r = (volatile s32 *)MX_UNCACHED(wr_result);
  if (mx_sif_call(&fio, FILEIO_CLOSE, (void *)a, 16, (void *)r, 4) < 0)
    return -1;
  return r[0];
}

/* DOPEN: the path (at a 16-byte boundary); DREAD: the IOP sends the entry
   (io_dirent_t: its stat, 40 bytes, then the name) by SIF DMA to buf. */
int mx_fio_dopen(const char *path) {
  if (fio_bind() < 0)
    return -1;
  static char dp_arg[256] MX_ALIGNED(64);
  char *a = MX_UNCACHED(dp_arg);
  u32 i = 0;
  for (; path[i] && i < sizeof dp_arg - 1; i++)
    a[i] = path[i];
  a[i] = 0;
  volatile s32 *r = (volatile s32 *)MX_UNCACHED(wr_result);
  if (mx_sif_call(&fio, FILEIO_DOPEN, a, sizeof dp_arg, (void *)r, 4) < 0)
    return -1;
  return r[0];
}

int mx_fio_dread(int fd, char *name, u32 room, u32 *size) {
  if (fio_bind() < 0)
    return -1;
  static struct {
    u32 mode, attr, size;
    u8 time[3][8];
    u32 hisize;
    char name[256];
    u32 unknown;
  } de MX_ALIGNED(64);
  static s32 dr_arg[4] MX_ALIGNED(64);
  mx_dcache_writeback(&de, sizeof de); /* no dirty lines for the DMA to meet */
  mx_dcache_invalidate(&de, sizeof de);
  volatile s32 *a = (volatile s32 *)MX_UNCACHED(dr_arg);
  a[0] = fd;
  a[1] = (s32)(uintptr_t)&de;
  volatile s32 *r = (volatile s32 *)MX_UNCACHED(wr_result);
  if (mx_sif_call(&fio, FILEIO_DREAD, (void *)a, 16, (void *)r, 4) < 0)
    return -1;
  if (r[0] <= 0)
    return r[0];
  volatile __typeof__(de) *e = MX_UNCACHED(&de);
  u32 i = 0;
  for (; room && i < room - 1 && i < sizeof de.name && e->name[i]; i++)
    name[i] = e->name[i];
  if (room)
    name[i] = 0;
  if (size)
    *size = e->size;
  return r[0];
}

int mx_fio_dclose(int fd) {
  if (fio_bind() < 0)
    return -1;
  static s32 dc_arg[4] MX_ALIGNED(64);
  volatile s32 *a = (volatile s32 *)MX_UNCACHED(dc_arg);
  a[0] = fd;
  volatile s32 *r = (volatile s32 *)MX_UNCACHED(wr_result);
  if (mx_sif_call(&fio, FILEIO_DCLOSE, (void *)a, 16, (void *)r, 4) < 0)
    return -1;
  return r[0];
}
