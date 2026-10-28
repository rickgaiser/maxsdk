/* VU1 vertex programs (HLSL): the VIF packets of their contexts and
   batches, and mesh chains (a draw's batches built once). Nothing here
   touches the hardware or the frame stream (host tests: tests/host). */

#include <mx/chain.h>
#include <mx/frame.h>

u64 *mx_shader_e_context(u64 *c, const struct mx_shader *s, u32 ctx, u32 prim, const void *uni) {
  /* one context: the batches before it end first */
  if (s->nctx == 1)
    c = mx_e_cnt(c, 0, 0, mx_vif_flushe());
  c = mx_e_cnt(c, s->ctx_qw, mx_vif_stcycl(1, 1),
               mx_vif_unpack(MX_UNPACK_V4_32, ctx, s->ctx_qw, 0));
  c = mx_e_qw(c, mx_giftag(0, 1, (s32)prim, MX_GIF_FLG_PACKED, s->nreg), s->regs);
  const u64 *u = uni;
  for (u32 i = 0; i < s->uni_qw; i++)
    c = mx_e_qw(c, u[2 * i], u[2 * i + 1]);
  /* the 1.0 that masked UNPACKs write (mx_shader_in.mask) */
  if (mx_shader_masks(s)) {
    c = mx_e_cnt(c, 1, 0, mx_vif_stcol());
    c = mx_e_w4(c, 0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u);
  }
  /* the values that they take from ROW (the header's ROW) */
  if (mx_shader_rows(s)) {
    c = mx_e_cnt(c, 1, 0, mx_vif_strow());
    c = mx_e_w4(c, s->row[0], s->row[1], s->row[2], s->row[3]);
  }
  /* the program's init (at the first batch) and each batch read it at ITOP */
  return mx_e_cnt(c, 0, 0, mx_vif_itop(ctx));
}

u64 *mx_shader_e_batch(u64 *c, const struct mx_shader *s, const void *const *streams, u32 n,
                       int first) {
  u32 m = (n + s->align - 1) / s->align * s->align;
  c = mx_e_cnt(c, 1, mx_vif_stcycl(1, 1), mx_vif_unpack(MX_UNPACK_V4_32, 0, 1, MX_UNPACK_FLG));
  c = mx_e_w4(c, n, 0, 0, n);
  /* input i of every vertex goes to quadword i of its item */
  for (u32 i = 0; i < s->nin; i++) {
    const struct mx_shader_in *in = &s->in[i];
#ifdef MX_DEBUG
    /* DMA drops the low address bits: a stream must start on a quadword */
    if ((uintptr_t)streams[i] & 15)
      mx__emit_overflow(__FILE__, __LINE__);
#endif
    if (in->mask)
      c = mx_e_cnt(c, 0, mx_vif_stmask(), in->mask);
    c = mx_e_ref(c, streams[i], m * in->bytes / 16, mx_vif_stcycl(s->item_qw, 1),
                 mx_vif_unpack(in->fmt, s->in_at + i, m,
                               MX_UNPACK_FLG | (in->usn ? MX_UNPACK_USN : 0) |
                                   (in->mask ? MX_UNPACK_MASK : 0)));
  }
  return mx_e_cnt(c, 0, mx_vif_stcycl(1, 1), first ? mx_vif_mscal(s->entry) : mx_vif_mscnt());
}

u64 *mx_shader_e_ibatch(u64 *c, const struct mx_shader *s, const void *const *streams,
                        const struct mx_ibatch *b, int first) {
  u32 m = (b->nv + s->align - 1) / s->align * s->align, iq = (b->ni + 15) / 16;
  /* the counts: vertices, -, index quadwords (four indices each), indices */
  c = mx_e_cnt(c, 1, mx_vif_stcycl(1, 1), mx_vif_unpack(MX_UNPACK_V4_32, 0, 1, MX_UNPACK_FLG));
  c = mx_e_w4(c, b->nv, 0, (b->ni + 3u) / 4, b->ni);
  for (u32 i = 0; i < s->nin; i++) {
    const struct mx_shader_in *in = &s->in[i];
    const u8 *at = (const u8 *)streams[i] + b->first * in->bytes;
#ifdef MX_DEBUG
    if ((uintptr_t)at & 15)
      mx__emit_overflow(__FILE__, __LINE__);
#endif
    if (in->mask)
      c = mx_e_cnt(c, 0, mx_vif_stmask(), in->mask);
    c = mx_e_ref(c, at, m * in->bytes / 16, mx_vif_stcycl(s->item_qw, 1),
                 mx_vif_unpack(in->fmt, s->in_at + i, m,
                               MX_UNPACK_FLG | (in->usn ? MX_UNPACK_USN : 0) |
                                   (in->mask ? MX_UNPACK_MASK : 0)));
  }
  /* the indices: 16 bytes are four quadwords of UNPACK V4-8 */
  c = mx_e_ref(c, b->idx, iq, mx_vif_stcycl(1, 1),
               mx_vif_unpack(MX_UNPACK_V4_8, s->idx_at, 4 * iq, MX_UNPACK_FLG | MX_UNPACK_USN));
  return mx_e_cnt(c, 0, 0, first ? mx_vif_mscal(s->entry) : mx_vif_mscnt());
}

u32 mx_shader_batches(const struct mx_shader *s, u32 prim, u32 n, u32 *size_, u32 *step_) {
  u32 type = prim & 7, step, size = s->max_n / s->align * s->align;
  if (type == MX_GS_PRIM_TRISTRIP) {
    /* the next batch starts with the last two vertices, on an aligned one */
    size = (s->max_n - 2) / s->align * s->align + 2;
    step = size - 2;
  } else {
    if (type == MX_GS_PRIM_TRIANGLE)
      while (size % 3)
        size -= s->align;
    step = size;
  }
  *size_ = size, *step_ = step;
  u32 nb = 0;
  for (u32 v = 0; v < n; v += step) {
    u32 k = n - v < size ? n - v : size;
    if (type == MX_GS_PRIM_TRISTRIP && v && k < 3)
      break;
    nb++;
    if (v + k >= n)
      break;
  }
  return nb;
}

u64 *mx_shader_e_draw(u64 *c, const struct mx_shader *s, u32 prim, const void *const *streams,
                      u32 n) {
  u32 size, step, nb = mx_shader_batches(s, prim, n, &size, &step);
  const void *at[MX_SHADER_MAX_IN];
  for (u32 b = 0, v = 0; b < nb; b++, v += step) {
    for (u32 i = 0; i < s->nin; i++)
      at[i] = (const u8 *)streams[i] + v * s->in[i].bytes;
    c = mx_shader_e_batch(c, s, at, n - v < size ? n - v : size, b == 0);
  }
  return c;
}

u64 *mx_shader_e_idraw(u64 *c, const struct mx_shader *s, const void *const *streams,
                       const struct mx_ibatch *b, u32 n, int first) {
  u32 am = s->align - 1;
  if (s->align & am) {
    for (u32 i = 0; i < n; i++)
      c = mx_shader_e_ibatch(c, s, streams, &b[i], first && i == 0);
    return c;
  }
  /* the packets of mx_shader_e_ibatch, from what a draw has once: each
     input's UNPACK but for its count, its stream's DMA address (linear: the
     batch's is first * bytes on) */
  u32 base[MX_SHADER_MAX_IN], bytes[MX_SHADER_MAX_IN], code[MX_SHADER_MAX_IN];
  for (u32 i = 0; i < s->nin; i++) {
    const struct mx_shader_in *in = &s->in[i];
    base[i] = mx_phys(streams[i]), bytes[i] = in->bytes;
    code[i] = mx_vif_unpack(in->fmt, s->in_at + i, 0,
                            MX_UNPACK_FLG | (in->usn ? MX_UNPACK_USN : 0) |
                                (in->mask ? MX_UNPACK_MASK : 0));
  }
  const u64 ref = mx_dmatag(MX_TAG_REF, 0, 0, 0), stcycl = mx_vif_stcycl(s->item_qw, 1);
  const u64 counts_lo = mx_dmatag(MX_TAG_CNT, 1, 0, 0);
  const u64 counts_hi =
      (u64)mx_vif_stcycl(1, 1) | (u64)mx_vif_unpack(MX_UNPACK_V4_32, 0, 1, MX_UNPACK_FLG) << 32;
  const u64 idx_hi = (u64)mx_vif_stcycl(1, 1) |
                     (u64)mx_vif_unpack(MX_UNPACK_V4_8, s->idx_at, 0, MX_UNPACK_FLG | MX_UNPACK_USN)
                         << 32;
  for (u32 i = 0; i < n; i++) {
    const struct mx_ibatch *k = &b[i];
    u32 m = (k->nv + am) & ~am, iq = (k->ni + 15u) / 16;
    c[0] = counts_lo, c[1] = counts_hi;
    c[2] = k->nv, c[3] = (u64)((k->ni + 3u) / 4) | (u64)k->ni << 32;
    c += 4;
    for (u32 j = 0; j < s->nin; j++) {
      if (s->in[j].mask)
        c = mx_e_cnt(c, 0, mx_vif_stmask(), s->in[j].mask);
      c[0] = ref | (m * bytes[j] / 16) | (u64)(base[j] + k->first * bytes[j]) << 32;
      c[1] = stcycl | (u64)(code[j] | (m & 0xFF) << 16) << 32;
      c += 2;
    }
    c = mx_e_qw(c, mx_dmatag(MX_TAG_REF, iq, k->idx, 0), idx_hi | (u64)((4 * iq) & 0xFF) << 48);
    c = mx_e_cnt(c, 0, 0, first && i == 0 ? mx_vif_mscal(s->entry) : mx_vif_mscnt());
  }
  return c;
}

/* ---- mesh chains --------------------------------------------------------------- */

u32 mx_shader_mesh_qw(const struct mx_shader *s, u32 prim, u32 n) {
  u32 size, step;
  return mx_shader_batches(s, prim, n, &size, &step) * mx_shader_batch_qw(s) + 1;
}

int mx_shader_mesh(struct mx_chain *ch, struct mx_pbuf *pb, const struct mx_shader *s, u32 prim,
                   const void *const *streams, u32 n) {
  mx_chain_begin(ch, pb);
  u64 *c = mx_pbuf_reserve(pb, mx_shader_mesh_qw(s, prim, n));
  if (!c) {
    ch->qwc = 0;
    return -1;
  }
  mx_pbuf_commit(pb, mx_shader_e_draw(c, s, prim, streams, n));
  return mx_chain_close(ch, pb, MX_TAG_RET);
}

int mx_shader_imesh(struct mx_chain *ch, struct mx_pbuf *pb, const struct mx_shader *s,
                    const void *const *streams, const struct mx_ibatch *b, u32 n) {
  mx_chain_begin(ch, pb);
  u64 *c = mx_pbuf_reserve(pb, mx_shader_imesh_qw(s, n));
  if (!c) {
    ch->qwc = 0;
    return -1;
  }
  mx_pbuf_commit(pb, mx_shader_e_idraw(c, s, streams, b, n, 1));
  return mx_chain_close(ch, pb, MX_TAG_RET);
}
