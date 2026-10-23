#ifndef MX_VIF_H
#define MX_VIF_H

/* VIF codes (EE User's Manual ch. 6). These build 32-bit words; use them with
   a packet builder (<mx/pkt.h>) or with your own cursor in hot loops. */

#include <mx/bits.h>
#include <mx/hw/formats.h>
#include <mx/types.h>

#define MX_VIF_IBIT 0x80000000u /* interrupt (and stop) after this code */

MX_INLINE u32 mx_vifcode(u32 cmd, u32 num, u32 imm) {
  return (u32)(MX_SET(VIFCODE_CMD, cmd) | MX_SET(VIFCODE_NUM, num) | MX_SET(VIFCODE_IMM, imm));
}

MX_INLINE u32 mx_vif_nop(void) { return 0; }
MX_INLINE u32 mx_vif_stcycl(u32 cl, u32 wl) { return mx_vifcode(MX_VIF_STCYCL, 0, cl | wl << 8); }
MX_INLINE u32 mx_vif_offset(u32 off) { return mx_vifcode(MX_VIF_OFFSET, 0, off); }
MX_INLINE u32 mx_vif_base(u32 base) { return mx_vifcode(MX_VIF_BASE, 0, base); }
MX_INLINE u32 mx_vif_itop(u32 v) { return mx_vifcode(MX_VIF_ITOP, 0, v); }
MX_INLINE u32 mx_vif_stmod(u32 mode) { return mx_vifcode(MX_VIF_STMOD, 0, mode); }
MX_INLINE u32 mx_vif_mskpath3(int mask) { return mx_vifcode(MX_VIF_MSKPATH3, 0, mask ? 0x8000 : 0); }
MX_INLINE u32 mx_vif_mark(u32 v) { return mx_vifcode(MX_VIF_MARK, 0, v); }
MX_INLINE u32 mx_vif_flushe(void) { return mx_vifcode(MX_VIF_FLUSHE, 0, 0); }
MX_INLINE u32 mx_vif_flush(void) { return mx_vifcode(MX_VIF_FLUSH, 0, 0); }
MX_INLINE u32 mx_vif_flusha(void) { return mx_vifcode(MX_VIF_FLUSHA, 0, 0); }
/* addr in 64-bit (instruction) units of micro memory */
MX_INLINE u32 mx_vif_mscal(u32 addr) { return mx_vifcode(MX_VIF_MSCAL, 0, addr); }
MX_INLINE u32 mx_vif_mscalf(u32 addr) { return mx_vifcode(MX_VIF_MSCALF, 0, addr); }
MX_INLINE u32 mx_vif_mscnt(void) { return mx_vifcode(MX_VIF_MSCNT, 0, 0); }
/* STMASK, STROW and STCOL are followed by 1 / 4 / 4 data words */
MX_INLINE u32 mx_vif_stmask(void) { return mx_vifcode(MX_VIF_STMASK, 0, 0); }
MX_INLINE u32 mx_vif_strow(void) { return mx_vifcode(MX_VIF_STROW, 0, 0); }
MX_INLINE u32 mx_vif_stcol(void) { return mx_vifcode(MX_VIF_STCOL, 0, 0); }
/* MPG of n instructions (1..256) to micro memory addr (64-bit units) */
MX_INLINE u32 mx_vif_mpg(u32 n, u32 addr) { return mx_vifcode(MX_VIF_MPG, n & 0xFF, addr); }
/* DIRECT / DIRECTHL of qwc quadwords (1..65536) to the GIF (PATH2) */
MX_INLINE u32 mx_vif_direct(u32 qwc) { return mx_vifcode(MX_VIF_DIRECT, 0, qwc & 0xFFFF); }
MX_INLINE u32 mx_vif_directhl(u32 qwc) { return mx_vifcode(MX_VIF_DIRECTHL, 0, qwc & 0xFFFF); }

/* UNPACK flags */
#define MX_UNPACK_USN 0x4000u /* zero-extend instead of sign-extend */
#define MX_UNPACK_FLG 0x8000u /* address relative to TOPS (double buffer) */
#define MX_UNPACK_MASK 0x10u  /* m bit: apply the STMASK write mask */

/* UNPACK num vectors of format fmt (MX_UNPACK_*) to VU memory addr
   (quadwords). flags: MX_UNPACK_USN | MX_UNPACK_FLG | MX_UNPACK_MASK. */
MX_INLINE u32 mx_vif_unpack(u32 fmt, u32 addr, u32 num, u32 flags) {
  return mx_vifcode(MX_VIF_UNPACK | fmt | (flags & MX_UNPACK_MASK), num & 0xFF,
                    (addr & 0x3FF) | (flags & (MX_UNPACK_USN | MX_UNPACK_FLG)));
}

/* Data words that follow an UNPACK of num vectors with write cycle
   cl = wl = 1. */
MX_INLINE u32 mx_unpack_words(u32 fmt, u32 num) {
  static const u8 bits[16] = {32, 16, 8, 0, 64, 32, 16, 0, 96, 48, 24, 0, 128, 64, 32, 16};
  return (bits[fmt & 15] * num + 31) / 32;
}

#endif
