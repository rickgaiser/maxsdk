#include <mx/rt.h>
#include <mx/tex.h>

void mx_rt_init(struct mx_rt *t, u32 fbp, u32 w, u32 h, u32 psm, u32 zbp, u32 zpsm) {
  *t = (struct mx_rt){.fbp = fbp,
                      .fbw = (w + 63) / 64,
                      .psm = psm,
                      .w = w,
                      .h = h,
                      .zbp = zbp,
                      .zpsm = zpsm,
                      .zmsk = zpsm == MX_RT_NOZ,
                      .ox = (2048 - w / 2) << 4,
                      .oy = (2048 - h / 2) << 4,
                      .sx1 = (u16)(w - 1),
                      .sy1 = (u16)(h - 1)};
}

u32 mx_rt_pages(u32 w, u32 h, u32 psm) {
  u32 pw, ph;
  mx_tex_page_size(psm, &pw, &ph);
  return (w + pw - 1) / pw * ((h + ph - 1) / ph);
}

u64 *mx_rt_e_regs(u64 *c, const struct mx_rt *t, int ctx) {
  u64 zbuf = t->zpsm == MX_RT_NOZ ? mx_gs_zbuf(0, MX_GS_PSM_Z32, 1)
                                  : mx_gs_zbuf(t->zbp, t->zpsm, (int)t->zmsk);
  c = mx_e_ad(c, mx_gs_ctx(MX_GS_REG_FRAME_1, ctx), mx_gs_frame(t->fbp, t->fbw, t->psm, t->fbmsk));
  c = mx_e_ad(c, mx_gs_ctx(MX_GS_REG_ZBUF_1, ctx), zbuf);
  c = mx_e_ad(c, mx_gs_ctx(MX_GS_REG_XYOFFSET_1, ctx), mx_gs_xyoffset(t->ox, t->oy));
  return mx_e_ad(c, mx_gs_ctx(MX_GS_REG_SCISSOR_1, ctx),
                 mx_gs_scissor(t->sx0, t->sx1, t->sy0, t->sy1));
}

u64 *mx_rt_e_env(u64 *c, const struct mx_rt *t, int ctx, int eop) {
  return mx_rt_e_regs(mx_e_ad_tag(c, 4, eop), t, ctx);
}

u64 *mx_rt_e_clear(u64 *c, const struct mx_rt *t, int ctx, u32 r, u32 g, u32 b, int eop) {
  c = mx_e_ad_tag(c, 4, eop);
  c = mx_e_ad(c, MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 0, 0, 0, 0, 0, ctx, 0));
  c = mx_e_ad(c, MX_GS_REG_RGBAQ, mx_gs_rgbaq(r, g, b, 0x80, 1.0f));
  c = mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(mx_rt_px(t, 0), mx_rt_py(t, 0), 0));
  return mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(mx_rt_px(t, t->w), mx_rt_py(t, t->h), 0));
}

static u32 log2_up(u32 n) {
  u32 k = 0;
  while ((1u << k) < n)
    k++;
  return k;
}

u64 mx_rt_tex0(const struct mx_rt *t, u32 tcc, u32 tfx) {
  u32 tw = log2_up(t->w), th = log2_up(t->h);
  if (tw > 10 || th > 10)
    return 0;
  /* TBP0 counts 64-word blocks, FBP 2048-word pages */
  return mx_gs_tex0(t->fbp * 32, t->fbw, t->psm, tw, th, tcc, tfx);
}
