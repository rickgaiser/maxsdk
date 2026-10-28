#include <mx/bits.h>
#include <mx/display.h>
#include <mx/hw/regs.h>

void mx_circuit_rt(struct mx_circuit *c, const struct mx_rt *t) {
  *c = (struct mx_circuit){
      .on = 1, .fbp = t->fbp, .fbw = t->fbw, .psm = t->psm, .w = t->w, .h = t->h};
}

void mx_display_init(struct mx_display *d, const struct mx_rt *t) {
  *d = (struct mx_display){.mmod = 1, .amod = 1};
  mx_circuit_rt(&d->c[1], t);
}

static u32 clamp(u32 v, u32 lo, u32 hi) { return v < lo ? lo : v > hi ? hi : v; }

u32 mx_circuit_magh(const struct mx_video_mode *m, const struct mx_circuit *c) {
  if (c->magh)
    return c->magh;
  return clamp(c->w ? m->vck_w / c->w : 1, 1, 16);
}

u32 mx_circuit_magv(const struct mx_video_mode *m, const struct mx_circuit *c) {
  if (c->magv)
    return c->magv;
  return clamp(c->h ? m->lines / (c->h * mx_video_line_lines(m)) : 1, 1, 4);
}

/* DISPFB1 and DISPFB2 (DISPLAY1 and DISPLAY2) have the same fields */
static u64 dispfb(const struct mx_circuit *c) {
  if (!c->on)
    return 0;
  return MX_SET(GS_DISPFB2_FBP, c->fbp) | MX_SET(GS_DISPFB2_FBW, c->fbw) |
         MX_SET(GS_DISPFB2_PSM, c->psm) | MX_SET(GS_DISPFB2_DBX, c->dbx) |
         MX_SET(GS_DISPFB2_DBY, c->dby);
}

/* The rectangle centred in the active area, then moved by (dx, dy). An
   interlaced picture moves by whole frames (2 lines), so its fields keep
   their order. */
static u64 display(const struct mx_video_mode *m, const struct mx_circuit *c) {
  if (!c->on)
    return 0;
  u32 mh = mx_circuit_magh(m, c), mv = mx_circuit_magv(m, c);
  s32 dw = (s32)(c->w * mh), dh = (s32)(c->h * mx_video_line_lines(m) * mv);
  s32 cy = ((s32)m->lines - dh) / 2;
  if (m->interlace)
    cy &= ~1;
  s32 x = (s32)m->dx + ((s32)m->vck_w - dw) / 2 + c->dx;
  s32 y = (s32)m->dy + cy + c->dy;
  return MX_SET(GS_DISPLAY2_DX, x < 0 ? 0 : x) | MX_SET(GS_DISPLAY2_DY, y < 0 ? 0 : y) |
         MX_SET(GS_DISPLAY2_MAGH, mh - 1) | MX_SET(GS_DISPLAY2_MAGV, mv - 1) |
         MX_SET(GS_DISPLAY2_DW, dw - 1) | MX_SET(GS_DISPLAY2_DH, dh - 1);
}

void mx_display_regs(const struct mx_video_mode *m, const struct mx_display *d,
                     u64 out[MX_DISP_NREGS]) {
  /* CRTMD: always 1 */
  out[MX_DISP_PMODE] = MX_SET(GS_PMODE_EN1, d->c[0].on) | MX_SET(GS_PMODE_EN2, d->c[1].on) |
                       MX_SET(GS_PMODE_CRTMD, 1) | MX_SET(GS_PMODE_MMOD, d->mmod) |
                       MX_SET(GS_PMODE_AMOD, d->amod) | MX_SET(GS_PMODE_SLBG, d->slbg) |
                       MX_SET(GS_PMODE_ALP, d->alp);
  out[MX_DISP_DISPFB1] = dispfb(&d->c[0]);
  out[MX_DISP_DISPLAY1] = display(m, &d->c[0]);
  out[MX_DISP_DISPFB2] = dispfb(&d->c[1]);
  out[MX_DISP_DISPLAY2] = display(m, &d->c[1]);
  out[MX_DISP_BGCOLOR] = d->bgcolor & 0xFFFFFF;
}
