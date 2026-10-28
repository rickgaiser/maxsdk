/* Chains (<mx/chain.h>) and the frame pipeline in chain mode.

   Part 1, VIF1 chains into VU1 data memory: a chain that calls two others,
   one of which calls a third (two levels); a chain sent on its own (its ret
   ends the transfer); and one queued by mx_dmaq_try_chain.

   Part 2, mx_frame_init_chain: an HLSL program (demos/model/mflat.hlsl) draws
   a strip of NV vertices (several batches) by mx_frame_shader_draw at the
   top, and the same strip built once as a mesh chain (mx_shader_mesh) by
   mx_frame_shader_call, moved down by its uniforms, COPIES times per frame
   (enough stream to go round the slots). A VU1 compute pass runs between
   them. The two strips must be the same pixels. */

#include "mflat.h"
#include "t.h"

extern const u8 mflat_vu[], mflat_vu_end[], dbl_vu[], dbl_vu_end[];

/* ---- part 1 ----------------------------------------------------------------- */

static u64 cbuf[64 * 2] MX_ALIGNED(64);
static u32 vu[32][4] MX_ALIGNED(16);

/* An UNPACK V4-32 of one quadword (v, v, v, v) to VU1 address at. */
static u64 *unpack1(u64 *c, u32 at, u32 v) {
  c = mx_e_cnt(c, 1, mx_vif_stcycl(1, 1), mx_vif_unpack(MX_UNPACK_V4_32, at, 1, 0));
  return mx_e_w4(c, v, v, v, v);
}

/* VU1 addresses 0..31 to zero. */
static void vu1_clear(void) {
  u64 *c = mx_e_end(cbuf, 32, mx_vif_stcycl(1, 1), mx_vif_unpack(MX_UNPACK_V4_32, 0, 32, 0));
  for (int i = 0; i < 32; i++)
    c = mx_e_qw(c, 0, 0);
  mx_dcache_writeback(cbuf, sizeof cbuf);
  mx_dma_chain(MX_CH_VIF1, cbuf, 1);
  CHECK_EQ(mx_dma_wait(MX_CH_VIF1), 0);
}

static void part1(void) {
  static u64 mem[128] MX_ALIGNED(64);
  struct mx_pbuf b;
  struct mx_chain x, y, top;
  mx_pbuf_init(&b, mem, sizeof mem);

  /* x: address 10 */
  mx_chain_begin(&x, &b);
  mx_pbuf_commit(&b, unpack1(mx_pbuf_reserve(&b, 2), 10, 0x10));
  CHECK_EQ(mx_chain_close(&x, &b, MX_TAG_RET), 0);
  /* y: address 20, calls x, address 21 */
  mx_chain_begin(&y, &b);
  u64 *c = mx_pbuf_reserve(&b, 5);
  c = unpack1(c, 20, 0x20);
  c = mx_e_call_chain(c, &x);
  c = unpack1(c, 21, 0x21);
  mx_pbuf_commit(&b, c);
  CHECK_EQ(mx_chain_close(&y, &b, MX_TAG_RET), 0);
  /* top: address 0, calls x and y, address 1, end */
  mx_chain_begin(&top, &b);
  c = mx_pbuf_reserve(&b, 6);
  c = unpack1(c, 0, 0xA0);
  c = mx_e_call_chain(c, &x);
  c = mx_e_call_chain(c, &y);
  c = unpack1(c, 1, 0xA1);
  mx_pbuf_commit(&b, c);
  CHECK_EQ(mx_chain_close(&top, &b, MX_TAG_END), 0);
  CHECK_EQ(b.overflow, 0);

  mx_vu_reset(2);
  vu1_clear();
  mx_chain_send(MX_CH_VIF1, &top, 1);
  CHECK_EQ(mx_dma_wait(MX_CH_VIF1), 0);
  mx_vu1_read(vu, 0, 32);
  CHECK_EQ(vu[0][0], 0xA0);
  CHECK_EQ(vu[1][3], 0xA1);
  CHECK_EQ(vu[10][0], 0x10);
  CHECK_EQ(vu[20][1], 0x20);
  CHECK_EQ(vu[21][2], 0x21);
  CHECK_EQ(vu[2][0], 0);
  CHECK_EQ(MX_GET(DMA_CHCR_ASP, MX_DMA_REG(MX_CH_VIF1, CHCR)), 0);

  /* y alone: the ret after its call of x ends the transfer */
  vu1_clear();
  mx_chain_send(MX_CH_VIF1, &y, 1);
  CHECK_EQ(mx_dma_wait(MX_CH_VIF1), 0);
  mx_vu1_read(vu, 0, 32);
  CHECK_EQ(vu[20][0], 0x20);
  CHECK_EQ(vu[10][0], 0x10);
  CHECK_EQ(vu[21][0], 0x21);
  CHECK_EQ(vu[0][0], 0);

  /* through a queue, as a handler would */
  static struct mx_dmaq q;
  vu1_clear();
  mx_dmaq_init(&q, MX_CH_VIF1);
  mx_event e;
  CHECK_EQ(mx_dmaq_try_chain(&q, top.start, 1, &e), 0);
  mx_event_wait(e);
  mx_dmaq_release(&q);
  mx_vu1_read(vu, 0, 32);
  CHECK_EQ(vu[0][0], 0xA0);
  CHECK_EQ(vu[21][0], 0x21);
}

/* ---- part 2 ----------------------------------------------------------------- */

#define FRAMES 6
#define NV 400
#define COPIES 120
#define DY 100

static u8 slots[64 * 1024] MX_ALIGNED(64);
static u64 mesh_mem[512] MX_ALIGNED(64);
static float pos[NV + 4][3] MX_ALIGNED(16);
static float uni[2][5][4] MX_ALIGNED(16);
static float cin[200][4] MX_ALIGNED(64), cout[200][4] MX_ALIGNED(64);
static u32 row[2][512] MX_ALIGNED(64);

/* mvp: GS units, moved down by dy lines; one colour */
static void uniforms(float (*u)[4], float dy) {
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 4; j++)
      u[i][j] = i == j ? 1.0f : 0.0f;
  u[3][0] = 2048 - 320, u[3][1] = 2048 - 112 + dy;
  u[4][0] = 0x40, u[4][1] = 0xC0, u[4][2] = 0x80, u[4][3] = 0x80;
}

static int part2(void) {
  const struct mx_shader s = MX_VU_SHADER(MFLAT, mflat_vu, mflat_vu_end);
  const u32 prim = (u32)mx_gs_prim(MX_GS_PRIM_TRISTRIP, 0, 0, 0, 0, 0, 0, 0, 0);
  /* a zigzag strip, 3 pixels a column, 60 lines tall */
  for (u32 i = 0; i < NV + 4; i++) {
    pos[i][0] = 20.0f + (float)(i / 2) * 3.0f;
    pos[i][1] = 10.0f + (float)(i & 1) * 60.0f;
    pos[i][2] = 0;
  }
  mx_dcache_writeback(pos, sizeof pos);
  uniforms(uni[0], 0);
  uniforms(uni[1], DY);
  mx_dcache_writeback(uni, sizeof uni);
  for (int i = 0; i < 200; i++)
    for (int j = 0; j < 4; j++)
      cin[i][j] = (float)(i * 4 + j);
  mx_dcache_writeback(cin, sizeof cin);
  const struct mx_kernel dbl = {.vu = 1,
                                .code = dbl_vu,
                                .code_bytes = (u32)(dbl_vu_end - dbl_vu),
                                .entry = 512,
                                .hdr_qw = 1,
                                .count_qw = 0,
                                .data_qw = 1,
                                .item_qw = 1};
  const u32 chdr[4] MX_ALIGNED(16) = {0, 0, 0, 0};

  /* the mesh chain, once */
  struct mx_pbuf pb;
  struct mx_chain mesh;
  const void *st[1] = {pos};
  mx_pbuf_init(&pb, mesh_mem, sizeof mesh_mem);
  CHECK_EQ(mx_shader_mesh(&mesh, &pb, &s, prim, st, NV), 0);
  u32 size, step;
  mx_printf("chain: mesh of %u vertices: %u batches, %u quadwords\n", NV,
            mx_shader_batches(&s, prim, NV, &size, &step), mesh.qwc);

  mx_vu_reset(2);
  mx_vu_forget(1);
  CHECK_EQ(mx_frame_init_chain(slots, sizeof slots, (void *)0x70002000, 0x2000), 0);
  int cbad = 0;
  for (int f = 0; f < FRAMES; f++) {
    mx_frame_begin(0x10, 0x10, 0x10);
    mx_frame_shader_setup(&s);
    mx_frame_shader_draw(&s, prim, uni[0], st, NV);
    for (int i = 0; i < 200; i++)
      cout[i][0] = 0;
    CHECK_EQ(mx_frame_vu1_compute(&dbl, chdr, cin, 200, cout, (void *)0x70000000, 0x2000), 0);
    for (int i = 0; i < 200; i++)
      for (int j = 0; j < 4; j++)
        cbad += cout[i][j] != 2 * cin[i][j];
    mx_frame_shader_setup(&s);
    for (int k = 0; k < COPIES; k++)
      mx_frame_shader_call(&s, prim, uni[1], &mesh);
    mx_frame_end();
  }
  u32 t0 = mx_count();
  while (mx_gs.shown < FRAMES && mx_count() - t0 < 300000000u)
    ;
  mx_frame_stop();
  mx_printf("chain: %u frames shown, %u pushes, missed %u, starved %u\n", mx_gs.shown,
            mx_frame_stats.pushes, mx_gs.missed, mx_gs.starved);
  CHECK_EQ(mx_gs.shown, FRAMES);
  CHECK_EQ(cbad, 0);
  /* more stream than the slots hold: they were reused */
  CHECK(mx_frame_stats.pushes > sizeof slots / 0x1000);

  /* a row through the middle of each strip */
  u32 fb = mx_gs.fbp[(FRAMES - 1) & 1];
  CHECK_EQ(mx_gs_download(row[0], MX_GS_PAGE(fb), mx_gs.fbw, MX_GS_PSM_CT32, 0, 40, 512, 1), 0);
  CHECK_EQ(mx_gs_download(row[1], MX_GS_PAGE(fb), mx_gs.fbw, MX_GS_PSM_CT32, 0, 40 + DY, 512, 1),
           0);
  int drawn = 0, diff = 0;
  for (int x = 0; x < 512; x++) {
    drawn += row[0][x] == 0x8080C040u;
    diff += row[0][x] != row[1][x];
  }
  mx_printf("chain: %d pixels of the strip in the row, %d differ\n", drawn, diff);
  CHECK(drawn >= 480);
  CHECK_EQ(diff, 0);
  return 0;
}

int main(void) {
  mx_gs_init(640, 224);
  part1();
  part2();
  return t_done("chain");
}
