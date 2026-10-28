/* Chains (<mx/chain.h>) and mesh chains (src/gs/shader.c): tags, overflow,
   and a mesh chain holding the same bytes as the draw's batches one by
   one. */

#include <mx/chain.h>
#include <mx/frame.h>
#include <string.h>
#include "test.h"

static u64 mem[2048] __attribute__((aligned(16)));
static u64 ref[2048] __attribute__((aligned(16)));

static u32 tag_id(const u64 *t) { return (u32)(t[0] >> 28) & 7; }
static u32 tag_qwc(const u64 *t) { return (u32)t[0] & 0xFFFF; }

static void chain_tags(void) {
  struct mx_pbuf b;
  struct mx_chain ch;
  mx_pbuf_init(&b, mem, 8 * 16);
  mx_chain_begin(&ch, &b);
  u64 *c = mx_pbuf_reserve(&b, 2);
  c = mx_e_cnt(c, 1, 0, 0);
  c = mx_e_qw(c, 1, 2);
  mx_pbuf_commit(&b, c);
  CHECK_EQ(mx_chain_close(&ch, &b, MX_TAG_RET), 0);
  CHECK_EQ(ch.start, mem);
  CHECK_EQ(ch.qwc, 3);
  CHECK_EQ(tag_id(mem + 4), MX_TAG_RET);
  CHECK_EQ(tag_qwc(mem + 4), 0);
  CHECK_EQ(mem[5], 0); /* VIF NOPs */
  CHECK_EQ(mx_chain_empty(&ch), 0);

  /* a CALL of it */
  u64 call[2];
  mx_e_call_chain(call, &ch);
  CHECK_EQ(tag_id(call), MX_TAG_CALL);
  CHECK_EQ(tag_qwc(call), 0);
  CHECK_EQ(call[0] >> 32, mx_phys(mem));
  CHECK_EQ(call[1], 0);

  /* an empty chain: only its ret */
  struct mx_chain e;
  mx_chain_begin(&e, &b);
  CHECK_EQ(mx_chain_close(&e, &b, MX_TAG_END), 0);
  CHECK_EQ(e.qwc, 1);
  CHECK_EQ(tag_id(mem + 6), MX_TAG_END);
  CHECK_EQ(mx_chain_empty(&e), 1);

  /* overflow: 4 quadwords left */
  CHECK_EQ(mx_pbuf_used_qw(&b), 4);
  struct mx_chain o;
  mx_chain_begin(&o, &b);
  CHECK_EQ(mx_pbuf_reserve(&b, 5) == 0, 1);
  CHECK_EQ(b.overflow, 1);
  CHECK_EQ(mx_chain_close(&o, &b, MX_TAG_RET), -1);
  CHECK_EQ(o.qwc, 0);
  CHECK_EQ(mx_chain_empty(&o), 1);
}

/* A program of two inputs (V3-32 position, V4-8 normal masked from COL),
   output into GIF registers; the layout numbers are made up. */
static struct mx_shader shader(u32 align) {
  struct mx_shader s;
  memset(&s, 0, sizeof s);
  s.entry = 6;
  s.buf_qw = 500;
  s.item_qw = 2;
  s.out_qw = 2;
  s.max_n = 124;
  s.align = align;
  s.nin = 2;
  s.in[0] = (struct mx_shader_in){MX_UNPACK_V3_32, 0, 12, 0};
  s.in[1] = (struct mx_shader_in){MX_UNPACK_V4_8, 1, 4, 0x40};
  s.in_at = 1;
  s.idx_at = 300;
  s.max_i = 96;
  return s;
}

static u8 pos[12 * 704] __attribute__((aligned(16)));
static u8 nrm[4 * 704] __attribute__((aligned(16)));

/* The batches one by one, as mx_frame_shader_draw sent them before mesh
   chains (its loop, copied). */
static u64 *batches_ref(u64 *c, const struct mx_shader *s, u32 prim, const void *const *streams,
                        u32 n) {
  u32 type = prim & 7, step, size = s->max_n / s->align * s->align;
  if (type == MX_GS_PRIM_TRISTRIP) {
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
    c = mx_shader_e_batch(c, s, at, k, v == 0);
    if (v + k >= n)
      break;
  }
  return c;
}

static void mesh_same(u32 prim, u32 n) {
  struct mx_shader s = shader(4);
  const void *st[2] = {pos, nrm};
  memset(mem, 0xAA, sizeof mem);
  memset(ref, 0x55, sizeof ref);
  struct mx_pbuf b;
  struct mx_chain ch;
  mx_pbuf_init(&b, mem, sizeof mem);
  CHECK_EQ(mx_shader_mesh(&ch, &b, &s, prim, st, n), 0);
  u64 *e = batches_ref(ref, &s, prim, st, n);
  u32 qw = (u32)(e - ref) / 2;
  CHECK_EQ(ch.qwc, qw + 1);
  CHECK_EQ(mx_shader_mesh_qw(&s, prim, n), qw + 1);
  CHECK_EQ(memcmp(mem, ref, (size_t)qw * 16), 0);
  CHECK_EQ(tag_id(mem + 2 * qw), MX_TAG_RET);
  CHECK_EQ(mx_chain_empty(&ch), n == 0);
}

static void imesh_same(u32 align) {
  struct mx_shader s = shader(align);
  const void *st[2] = {pos, nrm};
  static u8 idx[5][96] __attribute__((aligned(16)));
  struct mx_ibatch ib[5];
  for (u32 i = 0; i < 5; i++)
    ib[i] = (struct mx_ibatch){
        .first = 40 * i, .nv = (u16)(30 + 7 * i), .ni = (u16)(50 + 9 * i), .idx = idx[i]};
  memset(mem, 0xAA, sizeof mem);
  memset(ref, 0x55, sizeof ref);
  struct mx_pbuf b;
  struct mx_chain ch;
  mx_pbuf_init(&b, mem, sizeof mem);
  CHECK_EQ(mx_shader_imesh(&ch, &b, &s, st, ib, 5), 0);
  u64 *c = ref;
  for (u32 i = 0; i < 5; i++)
    c = mx_shader_e_ibatch(c, &s, st, &ib[i], i == 0);
  u32 qw = (u32)(c - ref) / 2;
  CHECK_EQ(qw, 5 * mx_shader_ibatch_qw(&s));
  CHECK_EQ(ch.qwc, qw + 1);
  CHECK_EQ(memcmp(mem, ref, (size_t)qw * 16), 0);
  /* a run that is not the draw's first: MSCNT */
  u64 *d = mx_shader_e_idraw(mem, &s, st, ib, 1, 0);
  CHECK_EQ((u32)(d[-1] >> 32) >> 24 & 0x7F, MX_VIF_MSCNT);
}

void test_chain(void) {
  chain_tags();
  mesh_same(MX_GS_PRIM_TRISTRIP, 0);
  mesh_same(MX_GS_PRIM_TRISTRIP, 3);
  mesh_same(MX_GS_PRIM_TRISTRIP, 122);
  mesh_same(MX_GS_PRIM_TRISTRIP, 123); /* a last batch of 1 vertex: dropped */
  mesh_same(MX_GS_PRIM_TRISTRIP, 700);
  mesh_same(MX_GS_PRIM_TRIANGLE, 699);
  mesh_same(MX_GS_PRIM_POINT, 500);
  imesh_same(4); /* the fast path */
  imesh_same(3); /* mx_shader_e_ibatch */
}
