#include <mx/debug.h>
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
  u32 ring_qw;
  int (*idle)(void);
  /* chain mode: each half is copied (fromSPR) into a slot in memory, and
     the slot is queued on VIF1 when the next copy starts (or the stream
     must go out now: kick) */
  int chain;
  u64 *slot[MX_FRAME_SLOTS];
  mx_event ev[MX_FRAME_SLOTS];
  u8 used[MX_FRAME_SLOTS];
  u32 nslot, next;
  int pending;    /* a slot is being copied into, not queued yet */
  int presenting; /* mx_frame_present: the frame has ended (its FINISH) */
  u32 pend;
  struct mx_dmaq q;
} f;

void mx_frame_vsync(int on) {
  f.novsync = !on;
  mx_gs.immediate = !on;
}

void mx_frame_idle(int (*fn)(void)) { f.idle = fn; }

/* The EE's work while it would wait: fn until it has none or the wait is
   over. Returns its cycles (idle, not waiting). */
static u32 idle_while(int (*wait)(u32), u32 arg) {
  u32 busy = 0;
  if (f.idle)
    while (wait(arg)) {
      u32 t1 = mx_count();
      int more = f.idle();
      busy += mx_count() - t1;
      if (!more)
        break;
    }
  mx_frame_stats.idle += busy;
  return busy;
}

static int ring_full(u32 qwc) { return mx_mfifo_used() + qwc >= f.ring_qw; }
static int slot_busy(u32 k) { return f.used[k] && !mx_event_done(f.ev[k]); }
static int not_ready(u32 arg) {
  (void)arg;
  return !mx_frame_ready();
}

/* The halves; chain mode keeps a quadword of each for the end tag. */
static u32 init_halves(void *spr, u32 spr_bytes, int chain) {
  u32 half = spr_bytes / 2 & ~15u;
  if (half > 0x2000)
    half = 0x2000;
  f.half[0] = spr;
  f.half[1] = (u64 *)((u8 *)spr + half);
  f.half_qw = half / 16;
  f.cur = 0;
  f.chain = chain;
  f.pos = f.half[0];
  f.end = f.half[0] + 2 * (f.half_qw - (u32)chain);
  mx_frame_stats.pushes = mx_frame_stats.wait_ring = mx_frame_stats.wait_buffer = 0;
  mx_frame_stats.qw = 0;
  mx_frame_stats.idle = 0;
  mx_gs.first = mx_gs.frames;
  mx_dma_wait(MX_CH_VIF1);
  return half;
}

void mx_frame_init(void *ring, u32 size, void *spr, u32 spr_bytes) {
  init_halves(spr, spr_bytes, 0);
  f.ring_qw = size / 16;
  mx_mfifo_init(MX_CH_VIF1, ring, size, 1);
}

int mx_frame_init_chain(void *mem, u32 bytes, void *spr, u32 spr_bytes) {
  u32 half = init_halves(spr, spr_bytes, 1);
  f.nslot = bytes / half;
  if (f.nslot > MX_FRAME_SLOTS)
    f.nslot = MX_FRAME_SLOTS;
  if (f.nslot < 2)
    return -1;
  /* the DMA writes the slots: no dirty lines of them may be written back
     over it */
  mx_dcache_invalidate(mem, f.nslot * half);
  for (u32 k = 0; k < f.nslot; k++) {
    f.slot[k] = (u64 *)((u8 *)mem + k * half);
    f.used[k] = 0;
  }
  f.next = 0;
  f.pending = 0;
  mx_dmaq_init(&f.q, MX_CH_VIF1);
  return 0;
}

/* Queue the slot being copied into, once its copy is done. */
static void submit(void) {
  if (!f.pending)
    return;
  mx_dma_wait(MX_CH_FROMSPR);
  f.ev[f.pend] = mx_dmaq_chain(&f.q, f.slot[f.pend], 1);
  f.used[f.pend] = 1;
  f.pending = 0;
}

/* Chain mode: end the current half (n quadwords) with an end tag and copy
   it into the next slot once that slot has been sent. */
static void chain_flush(u32 n) {
  f.pos = mx_e_end(f.pos, 0, 0, 0);
  submit();
  u32 k = f.next;
  f.next = (k + 1) % f.nslot;
  u32 t0 = mx_count(), busy = idle_while(slot_busy, k);
  while (slot_busy(k))
    ;
  mx_frame_stats.wait_ring += mx_count() - t0 - busy;
  mx_dma_from_spr(f.slot[k], f.half[f.cur], n + 1);
  f.pending = 1, f.pend = k;
}

/* Push the current half into the ring and switch to the other one. The
   push waits for the previous one (the other half), so the half we switch
   to is free. */
static void flush(void) {
  u32 n = (u32)(f.pos - f.half[f.cur]) / 2;
  if (!n)
    return;
  if (f.chain) {
    chain_flush(n);
  } else {
    u32 t0 = mx_count(), busy = idle_while(ring_full, n);
    mx_mfifo_push(f.half[f.cur], n);
    mx_frame_stats.wait_ring += mx_count() - t0 - busy;
  }
  mx_frame_stats.pushes++;
  mx_frame_stats.qw += n;
  f.cur ^= 1;
  f.pos = f.half[f.cur];
  f.end = f.pos + 2 * (f.half_qw - (u32)f.chain);
}

/* Everything built so far goes to VIF1 now. */
static void kick(void) {
  flush();
  submit();
}

void mx_frame_stop(void) {
  kick();
  if (f.chain) {
    mx_dmaq_release(&f.q);
  } else {
    mx_mfifo_push_wait();
    mx_mfifo_stop();
  }
  mx_gs.counting = 0;
}

u64 *mx_frame_reserve(u32 qwc) {
  if (f.pos + 2 * qwc > f.end)
    flush();
  return f.pos;
}

void mx_frame_commit(u64 *end) { f.pos = end; }

int mx_frame_ready(void) {
  /* frame n draws into the buffers of frame n - d, which are free once
     frame n - d + 1 is shown (vsync off: drawn, as it is shown at its
     FINISH) */
  u32 n = mx_gs.frames, d = mx_gs.ndisp ? mx_gs.ndisp : 2;
  if (n < d)
    return 1;
  return f.novsync ? mx_gs.frames_done >= n - d + 1 : mx_gs.shown >= n - d + 2;
}

u32 mx_frame_begin_n(void) {
  if (!mx_frame_ready()) {
    /* the buffer of frame n - 2 is shown until frame n - 1 replaces it;
       push what is built so far so the GS has it meanwhile */
    kick();
    u32 t0 = mx_count(), busy = idle_while(not_ready, 0);
    mx_trace(MX_TR_WAIT_BEGIN, 0x100);
    while (!mx_frame_ready())
      ;
    mx_trace(MX_TR_WAIT_END, 0x100);
    mx_frame_stats.wait_buffer += mx_count() - t0 - busy;
  }
  return mx_gs.frames;
}

int mx_frame_begin(u32 r, u32 g, u32 b) {
  int buf = (int)(mx_frame_begin_n() & 1);
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

void mx_frame_target(const struct mx_rt *t, int ctx) {
  /* the batches before it are drawn into the old target first */
  u64 *c = mx_frame_reserve(6);
  c = mx_e_cnt(c, 5, mx_vif_flush(), mx_vif_direct(5));
  mx_frame_commit(mx_rt_e_env(c, t, ctx, 1));
}

void mx_frame_texflush(void) {
  u64 *c = mx_frame_reserve(3);
  c = mx_e_cnt(c, 2, mx_vif_flush(), mx_vif_direct(2));
  c = mx_e_ad_tag(c, 1, 1);
  mx_frame_commit(mx_e_ad(c, MX_GS_REG_TEXFLUSH, 0));
}

void mx_frame_wait_path3(void) {
  u64 *c = mx_frame_reserve(1);
  mx_frame_commit(mx_e_cnt(c, 0, mx_vif_flusha(), 0));
}

/* The frame's FINISH. Counted before anything after it can be pushed: the
   FINISH interrupt only counts frames that were submitted. */
static void finish(void) {
  u64 *c = mx_frame_reserve(3);
  c = mx_e_cnt(c, 2, mx_vif_flush(), mx_vif_direct(2));
  c = mx_e_ad_tag(c, 1, 1);
  c = mx_e_ad(c, MX_GS_REG_FINISH, 1);
  mx_frame_commit(c);
  mx_gs.frames++;
  mx_gs.counting = 1;
}

void mx_frame_present(void) {
  finish();
  /* VIF1 stops here; the vblank handler (mx_gs) lets it go on */
  mx_frame_commit(
      mx_e_cnt(mx_frame_reserve(1), 0, 0, mx_vif_mark(mx_gs.frames & 0xFFFF) | MX_VIF_IBIT));
  f.presenting = 1;
}

void mx_frame_end(void) {
  if (!f.presenting)
    finish();
  f.presenting = 0;
  kick();
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

void mx_frame_shader_setup(const struct mx_shader *s) {
  /* the programs' layouts differ: the inputs of s, or the uniforms and the
     table of an indexed one, may be where the GIF still reads a packet of
     the last program */
  u64 *c = mx_frame_reserve(2);
  c = mx_e_cnt(c, 1, mx_vif_flush(), mx_vif_base(0));
  c = mx_e_vif4(c, mx_vif_offset(s->buf_qw), mx_vif_stcycl(1, 1), 0, 0);
  mx_frame_commit(c);
  mx_frame_mpg(s->code, s->code_bytes, s->entry);
}

/* The context of the last draw: 0 or 1 (by turns, with two). */
static u32 ctx_turn;

/* The context of a draw that has batches (a draw without must not take one:
   see llvm VUEntryABI.md), into the stream. */
static void draw_context(const struct mx_shader *s, u32 prim, const void *uni) {
  ctx_turn = s->nctx > 1 ? ctx_turn ^ 1 : 0;
  u32 ctx = mx_shader_ctx(s, ctx_turn);
  mx_frame_commit(mx_shader_e_context(mx_frame_reserve(mx_shader_context_qw(s)), s, ctx, prim, uni));
}

void mx_frame_shader_idraw(const struct mx_shader *s, u32 prim, const void *uni,
                           const void *const *streams, const struct mx_ibatch *b, u32 n) {
  if (!n)
    return;
  draw_context(s, prim, uni);
  /* a few batches per reserve: one does not take much of a half */
  u32 qw = mx_shader_ibatch_qw(s);
  for (u32 i = 0; i < n; i += 8) {
    u32 k = n - i < 8 ? n - i : 8;
    mx_frame_commit(mx_shader_e_idraw(mx_frame_reserve(k * qw), s, streams, b + i, k, i == 0));
  }
}

void mx_frame_shader_draw(const struct mx_shader *s, u32 prim, const void *uni,
                          const void *const *streams, u32 n) {
  if (!n)
    return;
  u32 size, step, nb = mx_shader_batches(s, prim, n, &size, &step);
  const void *at[MX_SHADER_MAX_IN];
  draw_context(s, prim, uni);
  for (u32 b = 0, v = 0; b < nb; b++, v += step) {
    for (u32 i = 0; i < s->nin; i++)
      at[i] = (const u8 *)streams[i] + v * s->in[i].bytes;
    u64 *c = mx_frame_reserve(mx_shader_batch_qw(s));
    mx_frame_commit(mx_shader_e_batch(c, s, at, n - v < size ? n - v : size, b == 0));
  }
}

void mx_frame_call(const struct mx_chain *ch) {
  if (!f.chain) {
    mx_printf("mx_frame_call: the frame stream is an MFIFO (mx_frame_init_chain)\n");
    return;
  }
  mx_frame_commit(mx_e_call_chain(mx_frame_reserve(1), ch));
}

void mx_frame_shader_call(const struct mx_shader *s, u32 prim, const void *uni,
                          const struct mx_chain *mesh) {
  /* no batches: no context (draw_context) */
  if (mx_chain_empty(mesh))
    return;
  if (f.chain)
    draw_context(s, prim, uni);
  mx_frame_call(mesh);
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
  kick();
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
