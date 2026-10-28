/* Does an MFIFO drain (VIF1, chain mode with TTE) follow call / ret to a
   chain in memory outside the ring? The EE User's Manual (5.5) limits the
   drain to cnt, ref, refs, refe and end tags; the frame pipeline's chain
   mode (mx_frame_init_chain) exists because of that. Three packets pushed
   into the ring, each: UNPACK 0xA0 to VU1 address 0, then a tag to the
   chain (UNPACK 0x10 to address 10, ret), then UNPACK 0xA1 to address 1:
     ref   the chain's data by a ref tag (the control: works by the manual)
     call  a CALL of the chain
     next  a NEXT to the chain (its ret then has nothing to return to)
   For each: which addresses were written, whether the ring drained, and the
   drain's CHCR (ASP, STR), TADR and D_STAT.CIS. Afterwards the drain is
   stopped, MFIFO mode left and VIF1 reset, so a case that wedges the drain
   does not stop the next. */

#include <mx/mx.h>

static u8 ring[4096] MX_ALIGNED(4096);
static u64 chain[2 * 4] MX_ALIGNED(64);
static u32 vu[16][4] MX_ALIGNED(16);
static u32 zero[16][4] MX_ALIGNED(64);

#define SPR ((u64 *)0x70000000)

static u64 *unpack1(u64 *c, u32 at, u32 v) {
  c = mx_e_cnt(c, 1, mx_vif_stcycl(1, 1), mx_vif_unpack(MX_UNPACK_V4_32, at, 1, 0));
  return mx_e_w4(c, v, v, v, v);
}

static void vu1_clear(void) {
  u64 t[2 * 2] MX_ALIGNED(64);
  t[0] = mx_dmatag(MX_TAG_REF, 16, zero, 0);
  t[1] = (u64)mx_vif_stcycl(1, 1) | (u64)mx_vif_unpack(MX_UNPACK_V4_32, 0, 16, 0) << 32;
  t[2] = mx_dmatag(MX_TAG_END, 0, 0, 0);
  t[3] = 0;
  mx_dcache_writeback(t, sizeof t);
  mx_dma_chain(MX_CH_VIF1, t, 1);
  mx_dma_wait(MX_CH_VIF1);
}

static void run(const char *name, u32 id) {
  mx_vif_reset(1);
  vu1_clear();
  mx_mfifo_init(MX_CH_VIF1, ring, sizeof ring, 1);
  u64 *c = SPR;
  c = unpack1(c, 0, 0xA0);
  if (id == MX_TAG_REF) /* the chain's UNPACK quadword, by ref */
    c = mx_e_tag(c, MX_TAG_REF, 1, chain + 2, chain[1], chain[1] >> 32);
  else
    c = mx_e_tag(c, id, 0, chain, 0, 0);
  c = unpack1(c, 1, 0xA1);
  c = mx_e_cnt(c, 0, 0, 0); /* not a ref last (manual 5.5) */
  mx_mfifo_push(SPR, (u32)(c - SPR) / 2);
  mx_mfifo_push_wait();
  u32 t0 = mx_count(), drained = 0;
  while (mx_count() - t0 < 30000000u) /* 0.1 s */
    if (!mx_mfifo_used() && !(MX_R32(MX_VIF_VIF1_STAT) & 0x3)) {
      drained = 1;
      break;
    }
  u32 chcr = MX_DMA_REG(MX_CH_VIF1, CHCR), tadr = MX_DMA_REG(MX_CH_VIF1, TADR);
  u32 cis = MX_R32(MX_D_STAT) >> MX_CH_VIF1 & 1, vstat = MX_R32(MX_VIF_VIF1_STAT);
  /* recover: stop the drain, leave MFIFO mode */
  MX_DMA_REG(MX_CH_VIF1, CHCR) = chcr & ~(u32)MX_CHCR_STR;
  MX_R32(MX_D_CTRL) &= ~(u32)MX_BIT(D_CTRL_MFD);
  MX_R32(MX_D_STAT) = 1u << MX_CH_VIF1;
  mx_vif_reset(1);
  mx_vu1_read(vu, 0, 16);
  mx_printf("p18 %s: written 0:%s 10:%s 1:%s, drained %d, CHCR %08x (ASP %u STR %u) TADR %08x "
            "(ring %08x, chain %08x) CIS %u VIF1_STAT %08x\n",
            name, vu[0][0] == 0xA0 ? "yes" : "no", vu[10][0] == 0x10 ? "yes" : "no",
            vu[1][0] == 0xA1 ? "yes" : "no", drained, chcr, (u32)MX_GET(DMA_CHCR_ASP, chcr),
            (u32)MX_GET(DMA_CHCR_STR, chcr), tadr, mx_phys(ring), mx_phys(chain), cis, vstat);
}

int main(void) {
  u64 *c = unpack1(chain, 10, 0x10);
  mx_e_tag(c, MX_TAG_RET, 0, 0, 0, 0);
  mx_dcache_writeback(chain, sizeof chain);
  mx_vu_reset(2);
  run("ref", MX_TAG_REF);
  run("call", MX_TAG_CALL);
  run("next", MX_TAG_NEXT);
  MX_TEST_END(0);
  return 0;
}
