#include <mx/dma.h>
#include <mx/frame.h>
#include <mx/trace.h>

struct mx_frame_stats mx_frame_stats;

static struct {
  u64 *half[2];
  u64 *pos, *end;
  u32 half_qw;
  int cur;
  int novsync;
} f;

void mx_frame_vsync(int on) {
  f.novsync = !on;
  mx_gs.immediate = !on;
}

void mx_frame_init(void *ring, u32 size, void *spr, u32 spr_bytes) {
  u32 half = spr_bytes / 2 & ~15u;
  if (half > 0x2000)
    half = 0x2000;
  f.half[0] = spr;
  f.half[1] = (u64 *)((u8 *)spr + half);
  f.half_qw = half / 16;
  f.cur = 0;
  f.pos = f.half[0];
  f.end = f.half[0] + 2 * f.half_qw;
  mx_frame_stats.pushes = mx_frame_stats.wait_ring = mx_frame_stats.wait_buffer = 0;
  mx_gs.first = mx_gs.frames;
  mx_dma_wait(MX_CH_VIF1);
  mx_mfifo_init(MX_CH_VIF1, ring, size, 1);
}

/* Push the current half into the ring and switch to the other one. The
   push waits for the previous one (the other half), so the half we switch
   to is free. */
static void flush(void) {
  u32 n = (u32)(f.pos - f.half[f.cur]) / 2;
  if (!n)
    return;
  u32 t0 = mx_count();
  mx_mfifo_push(f.half[f.cur], n);
  mx_frame_stats.wait_ring += mx_count() - t0;
  mx_frame_stats.pushes++;
  f.cur ^= 1;
  f.pos = f.half[f.cur];
  f.end = f.pos + 2 * f.half_qw;
}

void mx_frame_stop(void) {
  flush();
  mx_mfifo_push_wait();
  mx_mfifo_stop();
  mx_gs.counting = 0;
}

u64 *mx_frame_reserve(u32 qwc) {
  if (f.pos + 2 * qwc > f.end)
    flush();
  return f.pos;
}

void mx_frame_commit(u64 *end) { f.pos = end; }

int mx_frame_ready(void) {
  u32 n = mx_gs.frames;
  if (n < 2)
    return 1;
  return f.novsync ? mx_gs.frames_done >= n - 1 : mx_gs.shown >= n;
}

int mx_frame_begin(u32 r, u32 g, u32 b) {
  if (!mx_frame_ready()) {
    /* the buffer of frame n - 2 is shown until frame n - 1 replaces it;
       push what is built so far so the GS has it meanwhile */
    flush();
    u32 t0 = mx_count();
    mx_trace(MX_TR_WAIT_BEGIN, 0x100);
    while (!mx_frame_ready())
      ;
    mx_trace(MX_TR_WAIT_END, 0x100);
    mx_frame_stats.wait_buffer += mx_count() - t0;
  }
  int buf = (int)(mx_gs.frames & 1);
  u64 *c = mx_frame_direct(14);
  c = mx_gs_e_draw_env(c, buf, 0);
  c = mx_gs_e_clear(c, r, g, b, 1);
  mx_frame_commit(c);
  return buf;
}

u64 *mx_frame_direct(u32 qwc) {
  u64 *c = mx_frame_reserve(qwc + 1);
  return mx_e_cnt(c, qwc, 0, mx_vif_direct(qwc));
}

void mx_frame_wait_path3(void) {
  u64 *c = mx_frame_reserve(1);
  mx_frame_commit(mx_e_cnt(c, 0, mx_vif_flusha(), 0));
}

void mx_frame_end(void) {
  u64 *c = mx_frame_reserve(3);
  c = mx_e_cnt(c, 2, mx_vif_flush(), mx_vif_direct(2));
  c = mx_e_ad_tag(c, 1, 1);
  c = mx_e_ad(c, MX_GS_REG_FINISH, 1);
  mx_frame_commit(c);
  mx_gs.frames++;
  mx_gs.counting = 1;
  flush();
}

/* ---- VU1 ------------------------------------------------------------------- */

static u32 buf_qw(const struct mx_kernel *k) { return (1024 - k->data_qw) / 2; }

u32 mx_frame_vu1_max_items(const struct mx_kernel *k, u32 uni_qw) {
  return (buf_qw(k) - 2 - uni_qw) / (k->item_qw + k->out_qw);
}

void mx_frame_vu1_setup(const struct mx_kernel *k) {
  u64 *c = mx_frame_reserve(2);
  c = mx_e_cnt(c, 1, mx_vif_base(k->data_qw), mx_vif_offset(buf_qw(k)));
  c = mx_e_vif4(c, mx_vif_stcycl(1, 1), 0, 0, 0);
  mx_frame_commit(c);
}

void mx_frame_mpg(const void *code, u32 bytes, u32 addr) {
  if (mx_vu_resident(1, code, bytes, addr))
    return;
  u64 *c = mx_frame_reserve(bytes / 2048 + 4);
  c = mx_e_cnt(c, 0, mx_vif_flushe(), 0);
  mx_frame_commit(mx_vu_mpg_tags(c, code, bytes, addr));
  mx_vu_mark_resident(1, code, bytes, addr);
}

void mx_frame_vu1_header(const void *hdr, u32 hdr_qw) {
  u64 *c = mx_frame_reserve(hdr_qw + 1);
  c = mx_e_cnt(c, hdr_qw, mx_vif_flushe(), mx_vif_unpack(MX_UNPACK_V4_32, 0, hdr_qw, 0));
  const u64 *s = hdr;
  for (u32 i = 0; i < hdr_qw; i++)
    c = mx_e_qw(c, s[2 * i], s[2 * i + 1]);
  mx_frame_commit(c);
}

void mx_frame_vu1_batch(const struct mx_kernel *k, u64 giftag, u64 regs, const void *uni,
                        u32 uni_qw, const void *verts, u32 n) {
  u32 qw = n * k->item_qw;
  u64 *c = mx_frame_reserve(3 + uni_qw + (qw + 255) / 256);
  c = mx_e_cnt(c, 1 + uni_qw, 0, mx_vif_unpack(MX_UNPACK_V4_32, 0, 1 + uni_qw, MX_UNPACK_FLG));
  c = mx_e_qw(c, (giftag & ~0x7FFFull) | n, regs);
  const u64 *u = uni;
  for (u32 i = 0; i < uni_qw; i++)
    c = mx_e_qw(c, u[2 * i], u[2 * i + 1]);
  const u8 *d = verts;
  u32 at = 1 + uni_qw;
  while (qw) {
    u32 m = qw > 256 ? 256 : qw;
    c = mx_e_ref(c, d, m, 0, mx_vif_unpack(MX_UNPACK_V4_32, at, m, MX_UNPACK_FLG));
    d += 16 * m;
    at += m;
    qw -= m;
  }
  mx_frame_commit(mx_e_cnt(c, 0, 0, mx_vif_mscal(k->entry)));
}

/* ---- shaders ------------------------------------------------------------------ */

u64 *mx_shader_e_batch(u64 *c, const struct mx_shader *s, u32 prim, const void *uni,
                       const void *const *streams, u32 n) {
  u32 m = (n + s->align - 1) / s->align * s->align;
  c = mx_e_cnt(c, 1 + s->uni_qw, mx_vif_stcycl(1, 1),
               mx_vif_unpack(MX_UNPACK_V4_32, 0, 1 + s->uni_qw, MX_UNPACK_FLG));
  c = mx_e_qw(c, mx_giftag(n, 1, (s32)prim, MX_GIF_FLG_PACKED, s->nreg), s->regs);
  const u64 *u = uni;
  for (u32 i = 0; i < s->uni_qw; i++)
    c = mx_e_qw(c, u[2 * i], u[2 * i + 1]);
  /* input i of every vertex goes to quadword i of its item */
  for (u32 i = 0; i < s->nin; i++) {
    const struct mx_shader_in *in = &s->in[i];
#ifdef MX_DEBUG
    /* DMA drops the low address bits: a stream must start on a quadword */
    if ((uintptr_t)streams[i] & 15)
      mx__emit_overflow(__FILE__, __LINE__);
#endif
    c = mx_e_ref(c, streams[i], m * in->bytes / 16, mx_vif_stcycl(s->item_qw, 1),
                 mx_vif_unpack(in->fmt, 1 + s->uni_qw + i, m,
                               MX_UNPACK_FLG | (in->usn ? MX_UNPACK_USN : 0)));
  }
  return mx_e_cnt(c, 0, mx_vif_stcycl(1, 1), mx_vif_mscal(s->entry));
}

void mx_frame_shader_setup(const struct mx_shader *s) {
  u64 *c = mx_frame_reserve(2);
  c = mx_e_cnt(c, 1, mx_vif_base(0), mx_vif_offset(s->buf_qw));
  c = mx_e_vif4(c, mx_vif_stcycl(1, 1), 0, 0, 0);
  mx_frame_commit(c);
  mx_frame_mpg(s->code, s->code_bytes, s->entry);
}

void mx_frame_shader_draw(const struct mx_shader *s, u32 prim, const void *uni,
                          const void *const *streams, u32 n) {
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
  const void *at[MX_SHADER_MAX_IN];
  for (u32 v = 0; v < n; v += step) {
    u32 k = n - v < size ? n - v : size;
    if (type == MX_GS_PRIM_TRISTRIP && v && k < 3)
      break;
    for (u32 i = 0; i < s->nin; i++)
      at[i] = (const u8 *)streams[i] + v * s->in[i].bytes;
    u64 *c = mx_frame_reserve(mx_shader_batch_qw(s));
    mx_frame_commit(mx_shader_e_batch(c, s, prim, uni, at, k));
    if (v + k >= n)
      break;
  }
}

static u32 comp_hdr[16 * 4] MX_ALIGNED(16);

int mx_frame_vu1_compute(const struct mx_kernel *k, const void *hdr, const void *data, u32 n,
                         void *dst, void *spr, u32 spr_bytes) {
  mx_frame_commit(mx_e_cnt(mx_frame_reserve(1), 0, mx_vif_flush(), 0));
  mx_frame_mpg(k->code, k->code_bytes, k->entry);
  /* the header with the count, copied into the stream */
  const u32 *h = hdr;
  for (u32 i = 0; i < k->hdr_qw * 4; i++)
    comp_hdr[i] = h[i];
  comp_hdr[k->count_qw * 4] = n;
  u32 qw = n * k->item_qw;
  u64 *c = mx_frame_reserve(k->hdr_qw + 3 + (qw + 255) / 256);
  c = mx_e_cnt(c, k->hdr_qw, 0, mx_vif_unpack(MX_UNPACK_V4_32, 0, k->hdr_qw, 0));
  for (u32 i = 0; i < k->hdr_qw; i++)
    c = mx_e_w4(c, comp_hdr[4 * i], comp_hdr[4 * i + 1], comp_hdr[4 * i + 2], comp_hdr[4 * i + 3]);
  const u8 *d = data;
  u32 at = k->data_qw;
  while (qw) {
    u32 m = qw > 256 ? 256 : qw;
    c = mx_e_ref(c, d, m, 0, mx_vif_unpack(MX_UNPACK_V4_32, at, m, 0));
    d += 16 * m;
    at += m;
    qw -= m;
  }
  c = mx_e_cnt(c, 0, mx_vif_mscal(k->entry), mx_vif_flushe());
  /* stop after the program has ended, until the EE has read the results */
  c = mx_e_cnt(c, 0, mx_vif_nop() | MX_VIF_IBIT, 0);
  mx_frame_commit(c);
  flush();
  u32 t0 = mx_count();
  while (!(MX_R32(MX_VIF_VIF1_STAT) & MX_BIT(VIF_STAT_VIS)))
    if (mx_count() - t0 > 300000000u)
      return -1;
  mx_vu1_read_dma(dst, k->data_qw, n * k->item_qw, spr, spr_bytes);
  MX_R32(MX_VIF_VIF1_FBRST) = (u32)MX_BIT(VIF_FBRST_STC);
  return 0;
}

/* ---- textures --------------------------------------------------------------- */

static u32 tex_pk[2][64] MX_ALIGNED(64);

int mx_frame_texture(int slot, u32 dbp, u32 dbw, u32 psm, u32 w, u32 h, const void *data) {
  if (mx_dma_wait(MX_CH_GIF) < 0)
    return -1;
  struct mx_pkt p;
  mx_pkt_begin(&p, tex_pk[slot & 1], sizeof tex_pk[0], 0);
  mx_pkt_cnt(&p, 0, 0);
  mx_gs_upload(&p, dbp, dbw, psm, 0, 0, w, h, data);
  mx_pkt_end(&p, 0, 0);
  if (mx_pkt_finish(&p) < 0)
    return -1;
  mx_pkt_writeback(&p);
  mx_dma_chain(MX_CH_GIF, tex_pk[slot & 1], 0);
  return 0;
}
