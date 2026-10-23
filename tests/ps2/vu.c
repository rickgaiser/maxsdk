/* Phase 4: VU reset, VU0 micro mode through COP2 and through VIF0, VU1
   through VIF1, microcode residency, the T-bit interrupt, VIF1 double
   buffering with xtop + XGKICK, and VIF0 ITOP with xitop. */

#include "t.h"

extern const u8 dbl_vu[], dbl_vu_end[], tbit_vu[], tbit_vu_end[];
extern const u8 xtopkick_vu[], xtopkick_vu_end[], itop_vu[], itop_vu_end[];
extern const u8 dbl0db_vu[], dbl0db_vu_end[], dbl1db_vu[], dbl1db_vu_end[];

#define N 300
static float in[4 * N] MX_ALIGNED(64);
static float out[4 * N] MX_ALIGNED(64);
static u32 pk[1024] MX_ALIGNED(64);
static u32 px[32] MX_ALIGNED(64);

static void fill_in(void) {
  for (int i = 0; i < 4 * N; i++)
    in[i] = (float)(i % 97) - 40.0f;
  mx_dcache_writeback(in, sizeof in);
}

static int check_doubled(const float *o, int n) {
  int bad = 0;
  for (int i = 0; i < 4 * n; i++)
    bad += o[i] != 2 * in[i];
  return bad;
}

static void status(void) {
  u32 s = mx_status();
  mx_printf("COP0 Status %08x, CU2 %u\n", s, (s >> 30) & 1);
  CHECK(s & (1u << 30));
  mx_vu_reset(3);
  CHECK_EQ(mx_vpu_stat() & (MX_VPU_VBS0 | MX_VPU_VBS1), 0);
}

/* the doubling kernel: header of 1 quadword (count), items from 1 */
static struct mx_kernel kernel(int vu) {
  struct mx_kernel k = {.vu = vu,
                        .code = dbl_vu,
                        .code_bytes = (u32)(dbl_vu_end - dbl_vu),
                        .entry = 0,
                        .hdr_qw = 1,
                        .count_qw = 0,
                        .data_qw = 1,
                        .item_qw = 1};
  return k;
}

/* all items through one queue, in batches */
static void run_spr(int vu, int dma, int spr_readback);
static void run(int vu, int dma) { run_spr(vu, dma, 0); }

static void run_spr(int vu, int dma, int spr_readback) {
  struct mx_kernel k = kernel(vu);
  CHECK(mx_vu_load(vu, k.code, k.code_bytes, 0) >= 0);
  CHECK_EQ(mx_vu_load(vu, k.code, k.code_bytes, 0), 0); /* resident now */
  static struct mx_vuq qs[4];
  struct mx_vuq *qq = &qs[vu * 2 + dma];
  mx_vuq_init(qq, vu, dma);
  if (spr_readback)
    mx_vuq_readback_spr(qq, (void *)0x70000000, 0x4000);
  u32 batch = vu ? 200 : 100;
  u32 hdr[4] MX_ALIGNED(16) = {0, 0, 0, 0};
  memset(out, 0, sizeof out);
  for (u32 done = 0; done < N; done += batch) {
    u32 n = N - done < batch ? N - done : batch;
    mx_event e = mx_vuq_batch(qq, &k, hdr, &in[4 * done], n);
    CHECK(mx_vuq_wait(qq, e) == 0);
    mx_vuq_read(qq, &k, &out[4 * done], n);
  }
  mx_vuq_release(qq);
  int bad = check_doubled(out, N);
  if (bad)
    mx_printf("vu%d dma %d: %d wrong, out[0] %d in[0] %d\n", vu, dma, bad, (int)out[0],
              (int)in[0]);
  CHECK_EQ(bad, 0);
}

/* Double-buffered batches (VU0: ITOP + xitop; VU1: BASE/OFFSET + xtop):
   batch b+1 is sent before batch b's results are read. spr: VU1 results by
   DMA readback. */
static void run_db(int vu, int spr) {
  struct mx_kernel k = {.vu = vu,
                        .code = vu ? dbl1db_vu : dbl0db_vu,
                        .code_bytes = (u32)(vu ? dbl1db_vu_end - dbl1db_vu : dbl0db_vu_end - dbl0db_vu),
                        .entry = 0,
                        .hdr_qw = 1,
                        .count_qw = 0,
                        .data_qw = 1,
                        .item_qw = 1,
                        .dbuf = 1};
  mx_vu_load(vu, k.code, k.code_bytes, 0);
  static struct mx_vuq qq;
  mx_vuq_init(&qq, vu, 1);
  if (spr)
    mx_vuq_readback_spr(&qq, (void *)0x70000000, 0x4000);
  u32 batch = vu ? 100 : 60; /* several batches, at most a buffer each */
  CHECK(batch <= mx_kernel_max_items(&k));
  u32 hdr[4] MX_ALIGNED(16) = {0x1234, 0, 0, 0};
  memset(out, 0, sizeof out);
  mx_event prev = {0, 0};
  u32 prev_at = 0, prev_n = 0;
  for (u32 done = 0; done < N; done += batch) {
    u32 n = N - done < batch ? N - done : batch;
    mx_event e = mx_vuq_batch(&qq, &k, hdr, &in[4 * done], n);
#ifdef DB_DEBUG
    if (!vu && done <= batch) {
      mx_vu0_wait();
      u32 m[4 * 4] MX_ALIGNED(16);
      mx_vu0_read(m, 1, 2);
      mx_vu0_read(m + 8, 128, 2);
      mx_printf("db: after batch %u: buf0 cnt %u item0 %d | buf1 cnt %u item0 %d | in %d | ITOPS %u\n",
                done / batch, m[0], (int)((float *)m)[4], m[8], (int)((float *)m)[12],
                (int)in[4 * done], MX_R32(MX_VIF_VIF0_ITOPS));
    }
#endif
    if (prev.q) {
      CHECK(mx_vuq_wait(&qq, prev) == 0);
      mx_vuq_read(&qq, &k, &out[4 * prev_at], prev_n);
    }
    prev = e, prev_at = done, prev_n = n;
  }
  CHECK(mx_vuq_wait(&qq, prev) == 0);
  mx_vuq_read(&qq, &k, &out[4 * prev_at], prev_n);
  mx_vuq_release(&qq);
  int bad = check_doubled(out, N);
  if (bad)
    mx_printf("vu%d double buffered: %d wrong\n", vu, bad);
  CHECK_EQ(bad, 0);
}

static volatile u32 vu1_irqs;
static void on_vu1(void *a) { vu1_irqs++; }

static void tbit(void) {
  mx_vu_load(1, tbit_vu, (u32)(tbit_vu_end - tbit_vu), 64);
  mx_irq_intc(MX_INTC_VU1, on_vu1, 0);
  mx_vu_enable_tbit(1, 1);
  struct mx_pkt p;
  mx_pkt_begin(&p, MX_UNCACHED(pk), sizeof pk, MX_PKT_VIF);
  mx_pkt_end(&p, mx_vif_mscal(64), 0);
  mx_pkt_finish(&p);
  u32 n0 = vu1_irqs;
  mx_dma_chain(MX_CH_VIF1, pk, 1);
  mx_dma_wait(MX_CH_VIF1);
  u32 t0 = mx_count();
  while (vu1_irqs == n0 && mx_count() - t0 < 30000000u)
    ;
  mx_printf("t-bit: %u interrupts, VPU-STAT %08x\n", vu1_irqs - n0, mx_vpu_stat());
  CHECK_EQ(vu1_irqs - n0, 1);
  CHECK_EQ(mx_vpu_stat() & MX_VPU_VBS1, 0);
  mx_vu_enable_tbit(1, 0);
  mx_irq_intc(MX_INTC_VU1, 0, 0);
  /* VPU-STAT.VTS1 stays set after a T-bit stop, and the next MSCAL did not
     start VU1 on PCSX2: reset it */
  mx_vu_reset(2);
  CHECK_EQ(mx_vpu_stat() & 0x400, 0);
}

/* 8 batches through the VIF1 double buffer: each a GIF packet (a sprite)
   unpacked at TOPS, kicked by VU1 from xtop. No FLUSH between batches. */
static void double_buffer_kick(void) {
  mx_vu_load(1, xtopkick_vu, (u32)(xtopkick_vu_end - xtopkick_vu), 128);
  struct mx_pkt p;
  static u32 env[128] MX_ALIGNED(64);
  mx_pkt_begin(&p, env, sizeof env, 0);
  mx_pkt_cnt(&p, 0, 0);
  mx_gs_draw_env(&p, 0);
  mx_gs_clear(&p, 0, 0, 0);
  mx_pkt_end(&p, 0, 0);
  mx_pkt_finish(&p);
  mx_pkt_writeback(&p);
  mx_gs_send(env);
  mx_pkt_begin(&p, pk, sizeof pk, MX_PKT_VIF);
  mx_pkt_cnt(&p, mx_vif_stcycl(1, 1), mx_vif_base(200));
  mx_pkt_vif(&p, mx_vif_offset(100));
  static u32 g[8][16] MX_ALIGNED(16);
  for (int b = 0; b < 8; b++) {
    u64 *t = (u64 *)g[b];
    t[0] = mx_giftag(1, 1, (s32)mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 0, 0, 0, 0, 0, 0, 0),
                     MX_GIF_FLG_PACKED, 3);
    t[1] = mx_gif_regs3(MX_GIF_REG_RGBAQ, MX_GIF_REG_XYZ2, MX_GIF_REG_XYZ2);
    g[b][4] = 0x10 * (u32)b, g[b][5] = 0xFF, g[b][6] = 0x20, g[b][7] = 0x80;
    g[b][8] = mx_gs_px(20 + 40 * (u32)b), g[b][9] = mx_gs_py(180), g[b][10] = 0, g[b][11] = 0;
    g[b][12] = mx_gs_px(40 + 40 * (u32)b), g[b][13] = mx_gs_py(190), g[b][14] = 0,
    g[b][15] = 0;
    mx_pkt_unpack(&p, MX_UNPACK_V4_32, 0, 4, MX_UNPACK_FLG, g[b]);
    mx_pkt_vif(&p, mx_vif_mscal(128));
  }
  mx_pkt_vif(&p, mx_vif_flush());
  mx_pkt_end(&p, 0, 0);
  CHECK(mx_pkt_finish(&p) > 0);
  mx_pkt_writeback(&p);
  mx_dma_chain(MX_CH_VIF1, pk, 1);
  CHECK(mx_dma_wait(MX_CH_VIF1) == 0);
  /* FINISH over PATH3, then read the sprites back */
  static u32 f[16] MX_ALIGNED(64);
  mx_pkt_begin(&p, f, sizeof f, 0);
  mx_pkt_end(&p, 0, 0);
  mx_gs_finish_ad(&p);
  mx_pkt_finish(&p);
  mx_pkt_writeback(&p);
  u32 f0 = mx_gs.finish;
  mx_gs_send(f);
  CHECK(mx_gs_wait_finish(f0, 1) == 0);
  int bad = 0;
  for (u32 b = 0; b < 8; b++) {
    u32 x = 30 + 40 * b, x0 = x & ~31u;
    mx_gs_download(px, MX_GS_PAGE(mx_gs.fbp[0]), mx_gs.fbw, MX_GS_PSM_CT32, x0, 185, 32, 1);
    u32 want = 0x80000000u | 0x20u << 16 | 0xFFu << 8 | 0x10u * b;
    if (px[x - x0] != want) {
      mx_printf("sprite %u: %08x, expected %08x\n", b, px[x - x0], want);
      bad++;
    }
  }
  CHECK_EQ(bad, 0);
}

/* VIF0 ITOP, read by VU0 with xitop */
static void itop(void) {
  mx_vu_load(0, itop_vu, (u32)(itop_vu_end - itop_vu), 64);
  static u32 vp[16] MX_ALIGNED(64);
  struct mx_pkt p;
  mx_pkt_begin(&p, vp, sizeof vp, MX_PKT_VIF);
  mx_pkt_end(&p, mx_vif_itop(0x5A), mx_vif_mscal(64));
  mx_pkt_vif(&p, mx_vif_flushe());
  mx_pkt_finish(&p);
  mx_pkt_writeback(&p);
  mx_dma_chain(MX_CH_VIF0, vp, 1);
  CHECK(mx_dma_wait(MX_CH_VIF0) == 0);
  mx_vu0_wait();
  u32 v[4] MX_ALIGNED(16);
  mx_vu0_read(v, 10, 1);
  CHECK_EQ(v[0] & 0xFFFF, 0x5A);
}

#define STEP(x)                                                                            \
  do {                                                                                     \
    mx_printf("- %s\n", #x);                                                               \
    x;                                                                                     \
  } while (0)

int main(void) {
  mx_gs_init(640, 224);
  fill_in();
  STEP(status());
  STEP(run(0, 0));
  STEP(run(0, 1));
  STEP(run(1, 1));
  STEP(run_spr(1, 1, 1));
  STEP(run_db(0, 0));
  STEP(run_db(1, 0));
  STEP(run_db(1, 1));
  STEP(tbit());
  STEP(double_buffer_kick());
  STEP(itop());
  return t_done("vu");
}
