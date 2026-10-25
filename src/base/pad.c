/* Controllers through rom0:PADMAN (the protocol of the libpad EE client for
   the PADMAN in ROM). Each opened port has a 256-byte area in EE memory into
   which the IOP writes two records alternately; the one with the larger
   frame number is the newest. */

#include <mx/cpu.h>
#include <mx/pad.h>
#include <mx/sif.h>

#define PAD_SID 0x8000010F
#define PAD_SID2 0x8000011F
#define PAD_CMD_OPEN 0x80000100
#define PAD_CMD_SET_MAIN_MODE 0x80000105 /* port, slot, mode (1: analog), lock (3) */

#define PAD_STATE_STABLE 6
#define PAD_STATE_FINDCTP1 2

struct pad_record {
  u32 frame;
  u8 state, req_state, ok, unk7;
  u8 data[32]; /* data[1]: mode, data[2..3]: the buttons, 0 = held,
                  data[4..7] the sticks in analog mode */
  u32 length;
  u8 rest[20];
};

static struct mx_sif_client pad, pad2;
static struct pad_record area[2][4] MX_ALIGNED(64); /* 2 records + spare per port */
static s32 cmd[32] MX_ALIGNED(64);
static s32 res[32] MX_ALIGNED(64);
static int ready;
static u32 prev[2];
static u32 analog_polls[2];

int mx_pad_init(void) {
  if (ready)
    return 0;
  if (!mx_sif_ready() && mx_sif_init() < 0)
    return -1;
  /* a driver loaded by the loader (or by an earlier run) answers at once */
  if (mx_sif_bind_tries(&pad, PAD_SID, 2) < 0 || !pad.server) {
    if (mx_iop_load_module("rom0:SIO2MAN") < 0 || mx_iop_load_module("rom0:PADMAN") < 0)
      return -1;
    if (mx_sif_bind(&pad, PAD_SID) < 0)
      return -1;
  }
  if (mx_sif_bind(&pad2, PAD_SID2) < 0)
    return -1;
  for (int port = 0; port < 2; port++) {
    volatile struct pad_record *r = MX_UNCACHED(area[port]);
    for (int i = 0; i < 2; i++) {
      for (int k = 0; k < 32; k++)
        r[i].data[k] = 0xFF;
      r[i].frame = 0;
      r[i].length = 0;
    }
    mx_dcache_invalidate(area[port], sizeof area[port]);
    cmd[0] = PAD_CMD_OPEN;
    cmd[1] = port;
    cmd[2] = 0; /* slot */
    cmd[4] = (s32)(uintptr_t)area[port];
    if (mx_sif_call(&pad, 1, cmd, sizeof cmd, (void *)MX_UNCACHED(res), sizeof res) < 0)
      return -1;
  }
  ready = 1;
  return 0;
}

static volatile struct pad_record *newest(int port) {
  volatile struct pad_record *r = MX_UNCACHED(area[port]);
  return r[0].frame < r[1].frame ? &r[1] : &r[0];
}

int mx_pad_connected(int port) {
  if (!ready || (unsigned)port > 1)
    return 0;
  volatile struct pad_record *n = newest(port);
  return (n->state == PAD_STATE_STABLE || n->state == PAD_STATE_FINDCTP1) && n->length >= 4 &&
         n->data[0] == 0;
}

u32 mx_pad_buttons(int port) {
  if (!mx_pad_connected(port))
    return 0;
  volatile struct pad_record *n = newest(port);
  return ~(u32)(n->data[2] | n->data[3] << 8) & 0xFFFF;
}

/* A DualShock starts digital: once it is stable, ask for analog mode,
   locked (the ANALOG button does not switch it back); again every 128 polls
   while it is digital (a pad without analog mode says no). */
static void want_analog(int port, volatile struct pad_record *n) {
  if (n->state != PAD_STATE_STABLE || (n->data[1] >> 4) == 7 || analog_polls[port]++ % 128)
    return;
  cmd[0] = PAD_CMD_SET_MAIN_MODE;
  cmd[1] = port;
  cmd[2] = 0; /* slot */
  cmd[3] = 1;
  cmd[4] = 3;
  mx_sif_call(&pad, 1, cmd, sizeof cmd, (void *)MX_UNCACHED(res), sizeof res);
}

int mx_pad_sticks(int port, u8 st[4]) {
  for (int i = 0; i < 4; i++)
    st[i] = 128;
  if (!mx_pad_connected(port))
    return 0;
  volatile struct pad_record *n = newest(port);
  want_analog(port, n);
  /* data[1]: the type in the high nibble (7: analog) and the halfwords of
     data after it in the low one; the sticks are data[4..7] */
  if ((n->data[1] >> 4) != 7 || (n->data[1] & 15) < 3 || n->length < 8)
    return 0;
  for (int i = 0; i < 4; i++)
    st[i] = n->data[4 + i];
  return 1;
}

u32 mx_pad_pressed(int port) {
  if ((unsigned)port > 1)
    return 0;
  u32 b = mx_pad_buttons(port), p = b & ~prev[port];
  prev[port] = b;
  return p;
}
