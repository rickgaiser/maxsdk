#ifndef MX_PAD_H
#define MX_PAD_H

/* The controllers, through the IOP's pad driver (rom0:SIO2MAN and
   rom0:PADMAN, loaded unless a pad driver is already running). The IOP
   writes each port's state into EE memory every frame; reading it is a
   memory read. */

#include <mx/types.h>

#define MX_PAD_SELECT 0x0001
#define MX_PAD_L3 0x0002
#define MX_PAD_R3 0x0004
#define MX_PAD_START 0x0008
#define MX_PAD_UP 0x0010
#define MX_PAD_RIGHT 0x0020
#define MX_PAD_DOWN 0x0040
#define MX_PAD_LEFT 0x0080
#define MX_PAD_L2 0x0100
#define MX_PAD_R2 0x0200
#define MX_PAD_L1 0x0400
#define MX_PAD_R1 0x0800
#define MX_PAD_TRIANGLE 0x1000
#define MX_PAD_CIRCLE 0x2000
#define MX_PAD_CROSS 0x4000
#define MX_PAD_SQUARE 0x8000

/* Open port 0 and 1 (slot 0). Returns < 0 if the pad driver is not there. */
int mx_pad_init(void);
/* 1 if a pad on port (0/1) is connected and its state is being read. */
int mx_pad_connected(int port);
/* The buttons held on port (0/1), MX_PAD_* bits; 0 if no pad is connected. */
u32 mx_pad_buttons(int port);
/* Buttons pressed since the previous call for this port. */
u32 mx_pad_pressed(int port);
/* The sticks of port (0/1) when the pad is in analog mode (DualShock: the
   ANALOG button): st[0..3] = right x, right y, left x, left y, 0 .. 255 with
   the middle at 128 (y grows downwards). Returns 1, or 0 with 128s when the
   pad is digital or not connected. */
int mx_pad_sticks(int port, u8 st[4]);

#endif
