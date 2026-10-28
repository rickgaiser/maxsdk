#ifndef MX_GIF_H
#define MX_GIF_H

/* GIF tags and GS register values (GS User's Manual). The constructors take
   the fields in the manual's order; for anything else use
   MX_SET(GS_<REG>_<FIELD>, v). */

#include <mx/bits.h>
#include <mx/hw/formats.h>
#include <mx/types.h>

/* Low 64 bits of a GIF tag. prim < 0: no PRIM field (PRE = 0). */
MX_INLINE u64 mx_giftag(u32 nloop, int eop, s32 prim, u32 flg, u32 nreg) {
  return MX_SET(GIFTAG_NLOOP, nloop) | MX_SET(GIFTAG_EOP, eop) |
         (prim >= 0 ? MX_BIT(GIFTAG_PRE) | MX_SET(GIFTAG_PRIM, prim) : 0) |
         MX_SET(GIFTAG_FLG, flg) | MX_SET(GIFTAG_NREG, nreg & 15);
}

/* Register descriptor list (high 64 bits): r0 is sent first. */
MX_INLINE u64 mx_gif_regs2(u32 r0, u32 r1) { return (u64)r0 | (u64)r1 << 4; }
MX_INLINE u64 mx_gif_regs3(u32 r0, u32 r1, u32 r2) {
  return (u64)r0 | (u64)r1 << 4 | (u64)r2 << 8;
}
MX_INLINE u64 mx_gif_regs4(u32 r0, u32 r1, u32 r2, u32 r3) {
  return (u64)r0 | (u64)r1 << 4 | (u64)r2 << 8 | (u64)r3 << 12;
}

/* ---- GS register values --------------------------------------------------- */

MX_INLINE u64 mx_gs_prim(u32 prim, int iip, int tme, int fge, int abe, int aa1, int fst,
                         int ctxt, int fix) {
  return MX_SET(GS_PRIM_PRIM, prim) | MX_SET(GS_PRIM_IIP, iip) | MX_SET(GS_PRIM_TME, tme) |
         MX_SET(GS_PRIM_FGE, fge) | MX_SET(GS_PRIM_ABE, abe) | MX_SET(GS_PRIM_AA1, aa1) |
         MX_SET(GS_PRIM_FST, fst) | MX_SET(GS_PRIM_CTXT, ctxt) | MX_SET(GS_PRIM_FIX, fix);
}
MX_INLINE u64 mx_gs_rgbaq(u32 r, u32 g, u32 b, u32 a, float q) {
  union {
    float f;
    u32 u;
  } c = {q};
  return MX_SET(GS_RGBAQ_R, r) | MX_SET(GS_RGBAQ_G, g) | MX_SET(GS_RGBAQ_B, b) |
         MX_SET(GS_RGBAQ_A, a) | MX_SET(GS_RGBAQ_Q, c.u);
}
/* x, y in 12.4 fixed point (primitive coordinates, offset by XYOFFSET) */
MX_INLINE u64 mx_gs_xyz(u32 x, u32 y, u32 z) {
  return MX_SET(GS_XYZ_X, x) | MX_SET(GS_XYZ_Y, y) | MX_SET(GS_XYZ_Z, z);
}
MX_INLINE u64 mx_gs_uv(u32 u, u32 v) { return MX_SET(GS_UV_U, u) | MX_SET(GS_UV_V, v); }
MX_INLINE u64 mx_gs_frame(u32 fbp, u32 fbw, u32 psm, u32 fbmsk) {
  return MX_SET(GS_FRAME_FBP, fbp) | MX_SET(GS_FRAME_FBW, fbw) | MX_SET(GS_FRAME_PSM, psm) |
         MX_SET(GS_FRAME_FBMSK, fbmsk);
}
MX_INLINE u64 mx_gs_zbuf(u32 zbp, u32 psm, int zmsk) {
  return MX_SET(GS_ZBUF_ZBP, zbp) | MX_SET(GS_ZBUF_PSM, psm & 0xF) | MX_SET(GS_ZBUF_ZMSK, zmsk);
}
MX_INLINE u64 mx_gs_xyoffset(u32 ofx, u32 ofy) {
  return MX_SET(GS_XYOFFSET_OFX, ofx) | MX_SET(GS_XYOFFSET_OFY, ofy);
}
MX_INLINE u64 mx_gs_scissor(u32 x0, u32 x1, u32 y0, u32 y1) {
  return MX_SET(GS_SCISSOR_SCAX0, x0) | MX_SET(GS_SCISSOR_SCAX1, x1) |
         MX_SET(GS_SCISSOR_SCAY0, y0) | MX_SET(GS_SCISSOR_SCAY1, y1);
}
MX_INLINE u64 mx_gs_test(int ate, u32 atst, u32 aref, u32 afail, int date, int datm, int zte,
                         u32 ztst) {
  return MX_SET(GS_TEST_ATE, ate) | MX_SET(GS_TEST_ATST, atst) | MX_SET(GS_TEST_AREF, aref) |
         MX_SET(GS_TEST_AFAIL, afail) | MX_SET(GS_TEST_DATE, date) | MX_SET(GS_TEST_DATM, datm) |
         MX_SET(GS_TEST_ZTE, zte) | MX_SET(GS_TEST_ZTST, ztst);
}
MX_INLINE u64 mx_gs_tex0(u32 tbp, u32 tbw, u32 psm, u32 tw, u32 th, u32 tcc, u32 tfx) {
  return MX_SET(GS_TEX0_TBP0, tbp) | MX_SET(GS_TEX0_TBW, tbw) | MX_SET(GS_TEX0_PSM, psm) |
         MX_SET(GS_TEX0_TW, tw) | MX_SET(GS_TEX0_TH, th) | MX_SET(GS_TEX0_TCC, tcc) |
         MX_SET(GS_TEX0_TFX, tfx);
}
MX_INLINE u64 mx_gs_bitbltbuf(u32 sbp, u32 sbw, u32 spsm, u32 dbp, u32 dbw, u32 dpsm) {
  return MX_SET(GS_BITBLTBUF_SBP, sbp) | MX_SET(GS_BITBLTBUF_SBW, sbw) |
         MX_SET(GS_BITBLTBUF_SPSM, spsm) | MX_SET(GS_BITBLTBUF_DBP, dbp) |
         MX_SET(GS_BITBLTBUF_DBW, dbw) | MX_SET(GS_BITBLTBUF_DPSM, dpsm);
}
MX_INLINE u64 mx_gs_trxpos(u32 ssax, u32 ssay, u32 dsax, u32 dsay, u32 dir) {
  return MX_SET(GS_TRXPOS_SSAX, ssax) | MX_SET(GS_TRXPOS_SSAY, ssay) |
         MX_SET(GS_TRXPOS_DSAX, dsax) | MX_SET(GS_TRXPOS_DSAY, dsay) | MX_SET(GS_TRXPOS_DIR, dir);
}
MX_INLINE u64 mx_gs_trxreg(u32 w, u32 h) { return MX_SET(GS_TRXREG_RRW, w) | MX_SET(GS_TRXREG_RRH, h); }
MX_INLINE u64 mx_gs_tex1(u32 lcm, u32 mxl, u32 mmag, u32 mmin, u32 mtba, u32 l, u32 k) {
  return MX_SET(GS_TEX1_LCM, lcm) | MX_SET(GS_TEX1_MXL, mxl) | MX_SET(GS_TEX1_MMAG, mmag) |
         MX_SET(GS_TEX1_MMIN, mmin) | MX_SET(GS_TEX1_MTBA, mtba) | MX_SET(GS_TEX1_L, l) |
         MX_SET(GS_TEX1_K, k);
}
/* MIPTBP1 (levels 1-3) or MIPTBP2 (levels 4-6): the same layout */
MX_INLINE u64 mx_gs_miptbp(u32 tbp_a, u32 tbw_a, u32 tbp_b, u32 tbw_b, u32 tbp_c, u32 tbw_c) {
  return MX_SET(GS_MIPTBP1_TBP1, tbp_a) | MX_SET(GS_MIPTBP1_TBW1, tbw_a) |
         MX_SET(GS_MIPTBP1_TBP2, tbp_b) | MX_SET(GS_MIPTBP1_TBW2, tbw_b) |
         MX_SET(GS_MIPTBP1_TBP3, tbp_c) | MX_SET(GS_MIPTBP1_TBW3, tbw_c);
}
MX_INLINE u64 mx_gs_clamp(u32 wms, u32 wmt, u32 minu, u32 maxu, u32 minv, u32 maxv) {
  return MX_SET(GS_CLAMP_WMS, wms) | MX_SET(GS_CLAMP_WMT, wmt) | MX_SET(GS_CLAMP_MINU, minu) |
         MX_SET(GS_CLAMP_MAXU, maxu) | MX_SET(GS_CLAMP_MINV, minv) | MX_SET(GS_CLAMP_MAXV, maxv);
}
/* (A - B) * C >> 7 + D; A, B, D: 0 Cs, 1 Cd, 2 zero; C: 0 As, 1 Ad, 2 FIX */
MX_INLINE u64 mx_gs_alpha(u32 a, u32 b, u32 c, u32 d, u32 fix) {
  return MX_SET(GS_ALPHA_A, a) | MX_SET(GS_ALPHA_B, b) | MX_SET(GS_ALPHA_C, c) |
         MX_SET(GS_ALPHA_D, d) | MX_SET(GS_ALPHA_FIX, fix);
}
MX_INLINE u64 mx_gs_st(float s, float t) {
  union {
    float f[2];
    u32 u[2];
  } v = {{s, t}};
  return (u64)v.u[0] | (u64)v.u[1] << 32;
}
MX_INLINE u64 mx_gs_signal(u32 id, u32 mask) {
  return MX_SET(GS_SIGNAL_ID, id) | MX_SET(GS_SIGNAL_IDMSK, mask);
}
MX_INLINE u64 mx_gs_label(u32 id, u32 mask) {
  return MX_SET(GS_LABEL_ID, id) | MX_SET(GS_LABEL_IDMSK, mask);
}

#endif
