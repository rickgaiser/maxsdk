#ifndef MX_EMIT_H
#define MX_EMIT_H

/* Cursor emitters (decision D5a) for hot loops: no checks, no state. A
   cursor is a u64 * on a quadword boundary; every emitter writes whole
   quadwords and returns the advanced cursor. The caller sizes the buffer
   (MX_E_CHECK in debug builds). The bytes are the same as the checked
   builder's (<mx/pkt.h>, tests/host/test_emit.c). */

#include <mx/dma.h>
#include <mx/gif.h>
#include <mx/types.h>
#include <mx/vif.h>

/* One quadword: lo is bits 0-63, hi bits 64-127. Two sd: on the PS2 that
   is 2.57 cycles per A+D quadword into SPR, pcpyld + sq (asm) 5.04
   (docs/gif-gs.md). */
MX_INLINE u64 *mx_e_qw(u64 *c, u64 lo, u64 hi) {
  c[0] = lo;
  c[1] = hi;
  return c + 2;
}

/* Four 32-bit words (VIF codes and data). */
MX_INLINE u64 *mx_e_w4(u64 *c, u32 w0, u32 w1, u32 w2, u32 w3) {
  return mx_e_qw(c, (u64)w0 | (u64)w1 << 32, (u64)w2 | (u64)w3 << 32);
}

/* ---- DMA tags (source chain) ------------------------------------------- */

/* A DMA tag; c0, c1 are the VIF codes in its upper half (0 for GIF). qwc is
   the size of the data that follows (cnt/end) or at addr (ref/refs). */
MX_INLINE u64 *mx_e_tag(u64 *c, u32 id, u32 qwc, const void *addr, u32 c0, u32 c1) {
  return mx_e_qw(c, mx_dmatag(id, qwc, addr, 0), (u64)c0 | (u64)c1 << 32);
}
MX_INLINE u64 *mx_e_cnt(u64 *c, u32 qwc, u32 c0, u32 c1) {
  return mx_e_tag(c, MX_TAG_CNT, qwc, 0, c0, c1);
}
MX_INLINE u64 *mx_e_end(u64 *c, u32 qwc, u32 c0, u32 c1) {
  return mx_e_tag(c, MX_TAG_END, qwc, 0, c0, c1);
}
MX_INLINE u64 *mx_e_ref(u64 *c, const void *data, u32 qwc, u32 c0, u32 c1) {
  return mx_e_tag(c, MX_TAG_REF, qwc, data, c0, c1);
}
MX_INLINE u64 *mx_e_next(u64 *c, const void *next, u32 qwc, u32 c0, u32 c1) {
  return mx_e_tag(c, MX_TAG_NEXT, qwc, next, c0, c1);
}
MX_INLINE u64 *mx_e_call(u64 *c, const void *chain, u32 qwc, u32 c0, u32 c1) {
  return mx_e_tag(c, MX_TAG_CALL, qwc, chain, c0, c1);
}
MX_INLINE u64 *mx_e_ret(u64 *c, u32 qwc, u32 c0, u32 c1) {
  return mx_e_tag(c, MX_TAG_RET, qwc, 0, c0, c1);
}
/* Fill in the QWC of a cnt/end/next tag at tag once its data ends at c. */
MX_INLINE void mx_e_close(u64 *tag, const u64 *c) {
  tag[0] = (tag[0] & ~0xFFFFull) | (u64)((c - tag) / 2 - 1);
}

/* ---- GIF ----------------------------------------------------------------- */

MX_INLINE u64 *mx_e_giftag(u64 *c, u32 nloop, int eop, s32 prim, u32 flg, u32 nreg, u64 regs) {
  return mx_e_qw(c, mx_giftag(nloop, eop, prim, flg, nreg), regs);
}
/* A GIF tag for n A+D writes. */
MX_INLINE u64 *mx_e_ad_tag(u64 *c, u32 n, int eop) {
  return mx_e_giftag(c, n, eop, -1, MX_GIF_FLG_PACKED, 1, MX_GIF_REG_AD);
}
MX_INLINE u64 *mx_e_ad(u64 *c, u32 reg, u64 v) { return mx_e_qw(c, v, reg); }

/* PACKED vertex quadwords (GS User's Manual, PACKED mode). */
MX_INLINE u64 *mx_e_rgbaq(u64 *c, u32 r, u32 g, u32 b, u32 a) {
  return mx_e_w4(c, r, g, b, a);
}
MX_INLINE u64 *mx_e_st(u64 *c, float s, float t, float q) {
  union {
    float f[3];
    u32 u[3];
  } v = {{s, t, q}};
  return mx_e_w4(c, v.u[0], v.u[1], v.u[2], 0);
}
MX_INLINE u64 *mx_e_uv(u64 *c, u32 u, u32 v) { return mx_e_w4(c, u, v, 0, 0); }
/* x, y in 12.4 fixed point; adc = 1: no drawing kick (XYZ3) */
MX_INLINE u64 *mx_e_xyz2(u64 *c, u32 x, u32 y, u32 z, int adc) {
  return mx_e_qw(c, (u64)x | (u64)y << 32, (u64)z | (u64)(adc ? 1 : 0) << 47);
}

/* ---- VIF ----------------------------------------------------------------- */

/* Four VIF codes in one quadword (NOPs where not needed). */
MX_INLINE u64 *mx_e_vif4(u64 *c, u32 v0, u32 v1, u32 v2, u32 v3) {
  return mx_e_w4(c, v0, v1, v2, v3);
}

/* ---- checks ------------------------------------------------------------- */

/* Debug builds (MX_DEBUG): stop with a message if c ran past end. */
#ifdef MX_DEBUG
void mx__emit_overflow(const char *file, int line);
#define MX_E_CHECK(c, end)                                                                 \
  do {                                                                                     \
    if ((const u64 *)(c) > (const u64 *)(end))                                             \
      mx__emit_overflow(__FILE__, __LINE__);                                               \
  } while (0)
#else
#define MX_E_CHECK(c, end) ((void)0)
#endif

#endif
