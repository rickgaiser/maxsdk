/* Phase 3: one test per VIF code family (UNPACK formats, STMASK/STROW,
   BASE/OFFSET double buffering, ITOP, MARK, the i-bit interrupt, DIRECT)
   and per GIF mode (PACKED, REGLIST, IMAGE is in gs.c). */

#include "t.h"

#define VU1_MEM ((volatile u32 *)MX_VU1_DATA)

static u32 pk[1024] MX_ALIGNED(64);
static u32 px[32] MX_ALIGNED(64);
extern const u8 vuend_vu[], vuend_vu_end[];

static int vif1_send(struct mx_pkt *p) {
  if (mx_pkt_finish(p) <= 0)
    return -1;
  mx_pkt_writeback(p);
  mx_dma_chain(MX_CH_VIF1, mx_pkt_start(p), 1);
  return mx_dma_wait(MX_CH_VIF1);
}

static void vif1_begin(struct mx_pkt *p) {
  mx_pkt_begin(p, pk, sizeof pk, MX_PKT_VIF);
  mx_pkt_end(p, mx_vif_stcycl(1, 1), 0);
}

static u32 vu(u32 qw, u32 f) { return VU1_MEM[qw * 4 + f]; }

static void unpack_formats(void) {
  static const u32 s32[1] = {0x12345678};
  static const u16 v2_16[4] = {0x8001, 0x7FFF, 0xFFFF, 0x0002};
  static const u8 v4_8[8] = {0x81, 0x02, 0xFF, 0x7F, 1, 2, 3, 4};
  static const u32 v3_32[6] = {1, 2, 3, 4, 5, 6};
  static const u16 v4_5[2] = {0x801F, 0x03E0}; /* A=1 R=31; G=31 */
  struct mx_pkt p;
  vif1_begin(&p);
  mx_pkt_unpack(&p, MX_UNPACK_S_32, 400, 1, 0, s32);
  mx_pkt_unpack(&p, MX_UNPACK_V2_16, 401, 2, 0, v2_16);
  mx_pkt_unpack(&p, MX_UNPACK_V2_16, 403, 2, MX_UNPACK_USN, v2_16);
  mx_pkt_unpack(&p, MX_UNPACK_V4_8, 405, 2, MX_UNPACK_USN, v4_8);
  mx_pkt_unpack(&p, MX_UNPACK_V3_32, 407, 2, 0, v3_32);
  mx_pkt_unpack(&p, MX_UNPACK_V4_5, 409, 2, 0, v4_5);
  CHECK(vif1_send(&p) == 0);
  /* S-32: broadcast */
  CHECK_EQ(vu(400, 0), 0x12345678);
  CHECK_EQ(vu(400, 3), 0x12345678);
  /* V2-16 sign- and zero-extended */
  CHECK_EQ(vu(401, 0), 0xFFFF8001);
  CHECK_EQ(vu(401, 1), 0x7FFF);
  CHECK_EQ(vu(402, 0), 0xFFFFFFFF);
  CHECK_EQ(vu(403, 0), 0x8001);
  CHECK_EQ(vu(404, 0), 0xFFFF);
  /* V4-8 zero-extended */
  CHECK_EQ(vu(405, 0), 0x81);
  CHECK_EQ(vu(405, 2), 0xFF);
  CHECK_EQ(vu(406, 3), 4);
  /* V3-32 */
  CHECK_EQ(vu(407, 2), 3);
  CHECK_EQ(vu(408, 0), 4);
  /* V4-5: 5 bits << 3, A bit << 7 */
  CHECK_EQ(vu(409, 0), 31 << 3);
  CHECK_EQ(vu(409, 3), 0x80);
  CHECK_EQ(vu(410, 1), 31 << 3);
}

static void mask_row(void) {
  static const u32 v3[3] = {10, 20, 30};
  struct mx_pkt p;
  vif1_begin(&p);
  mx_pkt_vif(&p, mx_vif_strow());
  mx_pkt_w32(&p, 100), mx_pkt_w32(&p, 200), mx_pkt_w32(&p, 300), mx_pkt_w32(&p, 0x3F800000);
  /* x, y, z from the data, w from ROW */
  mx_pkt_vif(&p, mx_vif_stmask());
  mx_pkt_w32(&p, 1u << 6);
  mx_pkt_unpack(&p, MX_UNPACK_V3_32, 420, 1, MX_UNPACK_MASK, v3);
  /* offset mode: data + ROW */
  mx_pkt_vif(&p, mx_vif_stmod(1));
  mx_pkt_unpack(&p, MX_UNPACK_V3_32, 421, 1, 0, v3);
  mx_pkt_vif(&p, mx_vif_stmod(0));
  CHECK(vif1_send(&p) == 0);
  CHECK_EQ(vu(420, 0), 10);
  CHECK_EQ(vu(420, 3), 0x3F800000);
  CHECK_EQ(vu(421, 0), 110);
  CHECK_EQ(vu(421, 2), 330);
}

static void double_buffer(void) {
  static const u32 a[4] = {0xAAAA0000, 1, 2, 3}, b[4] = {0xBBBB0000, 1, 2, 3};
  struct mx_pkt p;
  vif1_begin(&p);
  mx_pkt_mpg(&p, vuend_vu, (u32)(vuend_vu_end - vuend_vu), 64);
  mx_pkt_vif(&p, mx_vif_base(100));
  mx_pkt_vif(&p, mx_vif_offset(40));
  mx_pkt_unpack(&p, MX_UNPACK_V4_32, 0, 1, MX_UNPACK_FLG, a);
  mx_pkt_vif(&p, mx_vif_mscal(64));
  mx_pkt_unpack(&p, MX_UNPACK_V4_32, 0, 1, MX_UNPACK_FLG, b);
  mx_pkt_vif(&p, mx_vif_flushe());
  CHECK(vif1_send(&p) == 0);
  mx_printf("double buffer: TOPS %u TOP %u\n", MX_R32(MX_VIF_VIF1_TOPS), MX_R32(MX_VIF_VIF1_TOP));
  CHECK_EQ(vu(100, 0), 0xAAAA0000);
  CHECK_EQ(vu(140, 0), 0xBBBB0000);
  CHECK_EQ(MX_R32(MX_VIF_VIF1_TOP), 100);
  CHECK_EQ(MX_R32(MX_VIF_VIF1_TOPS), 140);
}

static void itop_mark(void) {
  struct mx_pkt p;
  vif1_begin(&p);
  mx_pkt_vif(&p, mx_vif_itop(0x155));
  mx_pkt_vif(&p, mx_vif_mark(0xBEEF));
  CHECK(vif1_send(&p) == 0);
  CHECK_EQ(MX_R32(MX_VIF_VIF1_ITOPS), 0x155);
  CHECK_EQ(MX_R32(MX_VIF_VIF1_MARK), 0xBEEF);
}

static volatile u32 vif1_irqs;
static void on_vif1(void *a) { vif1_irqs++; }

static void ibit(void) {
  mx_irq_intc(MX_INTC_VIF1, on_vif1, 0);
  struct mx_pkt p;
  vif1_begin(&p);
  mx_pkt_vif(&p, mx_vif_mark(0x1111));
  mx_pkt_vif(&p, mx_vif_nop() | MX_VIF_IBIT);
  mx_pkt_vif(&p, mx_vif_mark(0x2222));
  CHECK(mx_pkt_finish(&p) > 0);
  mx_pkt_writeback(&p);
  u32 n0 = vif1_irqs;
  mx_dma_chain(MX_CH_VIF1, mx_pkt_start(&p), 1);
  u32 t0 = mx_count();
  while (vif1_irqs == n0 && mx_count() - t0 < 30000000u)
    ;
  CHECK_EQ(vif1_irqs - n0, 1);
  u32 stat = MX_R32(MX_VIF_VIF1_STAT);
  mx_printf("i-bit: STAT %08x MARK %04x\n", stat, MX_R32(MX_VIF_VIF1_MARK));
  CHECK(stat & MX_BIT(VIF_STAT_VIS));
  CHECK_EQ(MX_R32(MX_VIF_VIF1_MARK), 0x1111);
  /* resume */
  MX_R32(MX_VIF_VIF1_FBRST) = (u32)MX_BIT(VIF_FBRST_STC);
  CHECK(mx_dma_wait(MX_CH_VIF1) == 0);
  CHECK_EQ(MX_R32(MX_VIF_VIF1_MARK), 0x2222);
  mx_irq_intc(MX_INTC_VIF1, 0, 0);
}

static u32 pixel(u32 x, u32 y) {
  u32 x0 = x & ~31u;
  mx_gs_download(px, MX_GS_PAGE(mx_gs.fbp[0]), mx_gs.fbw, MX_GS_PSM_CT32, x0, y, 32, 1);
  return px[x - x0];
}

static void finish_path3(void) {
  static u32 f[16] MX_ALIGNED(64);
  struct mx_pkt p;
  mx_pkt_begin(&p, f, sizeof f, 0);
  mx_pkt_end(&p, 0, 0);
  mx_gs_finish_ad(&p);
  mx_pkt_finish(&p);
  mx_pkt_writeback(&p);
  u32 f0 = mx_gs.finish;
  mx_gs_send(f);
  CHECK(mx_gs_wait_finish(f0, 1) == 0);
}

static void path2_direct(void) {
  struct mx_pkt p;
  vif1_begin(&p);
  mx_pkt_direct(&p, 9 + 5); /* draw_env 9 qw, clear 5 qw */
  mx_gs_draw_env(&p, 0);
  mx_gs_clear(&p, 0, 0, 0);
  mx_pkt_direct(&p, 5);
  mx_pkt_ad_begin(&p, 4, 1);
  mx_pkt_ad(&p, MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 0, 0, 0, 0, 0, 0, 0));
  mx_pkt_ad(&p, MX_GS_REG_RGBAQ, mx_gs_rgbaq(0, 0, 255, 0x80, 1.0f));
  mx_pkt_ad(&p, MX_GS_REG_XYZ2, mx_gs_xyz(mx_gs_px(40), mx_gs_py(40), 0));
  mx_pkt_ad(&p, MX_GS_REG_XYZ2, mx_gs_xyz(mx_gs_px(60), mx_gs_py(50), 0));
  CHECK(vif1_send(&p) == 0);
  finish_path3();
  CHECK_EQ(pixel(50, 45), 0x80FF0000u);
  CHECK_EQ(pixel(10, 10), 0x80000000u);
}

static void gif_reglist(void) {
  static u32 g[64] MX_ALIGNED(64);
  struct mx_pkt p;
  mx_pkt_begin(&p, g, sizeof g, 0);
  mx_pkt_end(&p, 0, 0);
  /* REGLIST: 64-bit register data, NREG 4, NLOOP 1 (an even number of
     registers, so no padding) */
  mx_pkt_giftag(&p, 1, 1, (s32)mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 0, 0, 0, 0, 0, 0, 0),
                MX_GIF_FLG_REGLIST, 4,
                mx_gif_regs4(MX_GIF_REG_RGBAQ, MX_GIF_REG_XYZ2, MX_GIF_REG_XYZ2, MX_GIF_REG_NOP));
  mx_pkt_w64(&p, mx_gs_rgbaq(255, 255, 0, 0x80, 1.0f));
  mx_pkt_w64(&p, mx_gs_xyz(mx_gs_px(100), mx_gs_py(100), 0));
  mx_pkt_w64(&p, mx_gs_xyz(mx_gs_px(120), mx_gs_py(110), 0));
  mx_pkt_w64(&p, 0);
  CHECK(mx_pkt_finish(&p) > 0);
  mx_pkt_writeback(&p);
  mx_gs_send(g);
  finish_path3();
  CHECK_EQ(pixel(110, 105), 0x8000FFFFu);
}

/* MSKPATH3: while VIF1 masks PATH3, a GIF-channel transfer does not get
   through; unmasking lets it finish. */
static void mskpath3(void) {
  struct mx_pkt p;
  vif1_begin(&p);
  mx_pkt_vif(&p, mx_vif_mskpath3(1));
  CHECK(vif1_send(&p) == 0);
  static u32 g[64] MX_ALIGNED(64);
  struct mx_pkt q;
  mx_pkt_begin(&q, g, sizeof g, 0);
  mx_pkt_end(&q, 0, 0);
  mx_pkt_ad_begin(&q, 1, 1);
  mx_pkt_ad(&q, MX_GS_REG_LABEL, mx_gs_label(0x3333, 0xFFFFFFFFu));
  mx_pkt_finish(&q);
  mx_pkt_writeback(&q);
  mx_dma_chain(MX_CH_GIF, g, 0);
  mx_delay_us(200);
  int held = (u32)(MX_R64(MX_GS_SIGLBLID) >> 32) != 0x3333;
  mx_printf("mskpath3: PATH3 %s while masked\n", held ? "held" : "NOT held");
  CHECK(held);
  vif1_begin(&p);
  mx_pkt_vif(&p, mx_vif_mskpath3(0));
  CHECK(vif1_send(&p) == 0);
  CHECK(mx_dma_wait(MX_CH_GIF) == 0);
  mx_delay_us(50);
  CHECK_EQ(MX_R64(MX_GS_SIGLBLID) >> 32, 0x3333);
}

#define STEP(x)                                                                            \
  do {                                                                                     \
    mx_printf("- %s\n", #x);                                                               \
    x;                                                                                     \
  } while (0)

int main(void) {
  mx_gs_init(640, 224);
  MX_R32(MX_VIF_VIF1_FBRST) = 1;
  STEP(unpack_formats());
  STEP(mask_row());
  STEP(double_buffer());
  STEP(itop_mark());
  STEP(ibit());
  STEP(path2_direct());
  STEP(gif_reglist());
  STEP(mskpath3());
  return t_done("vif");
}
