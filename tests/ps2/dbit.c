/* Phase 4: the VU1 D bit. Measured on the PS2 (docs/interrupts.md): with
   FBRST.DE1 set, a D bit hangs the PS2, also when the VU1 interrupt handler
   resets VU1 (PCSX2 ignores it). So it is not used. This test checks the
   safe case: with DE1 clear, VU1 runs past the D bit to its E bit, and no
   VU1 interrupt arrives. */

#include "t.h"

extern const u8 dbit_vu[], dbit_vu_end[];

static u32 pk[64] MX_ALIGNED(64);
static volatile u32 irqs;
static void on_vu1(void *a) { irqs++; }

int main(void) {
  mx_dma_init();
  mx_vu_reset(3);
  mx_vu_load(1, dbit_vu, (u32)(dbit_vu_end - dbit_vu), 64);
  mx_irq_intc(MX_INTC_VU1, on_vu1, 0);
  mx_vu_enable_dbit(1, 0);
  struct mx_pkt p;
  mx_pkt_begin(&p, MX_UNCACHED(pk), sizeof pk, MX_PKT_VIF);
  mx_pkt_end(&p, mx_vif_mscal(64), mx_vif_flushe());
  mx_pkt_finish(&p);
  mx_dma_chain(MX_CH_VIF1, pk, 1);
  CHECK(mx_dma_wait(MX_CH_VIF1) == 0);
  u32 t0 = mx_count();
  while ((mx_vpu_stat() & MX_VPU_VBS1) && mx_count() - t0 < 30000000u)
    ;
  mx_delay_us(100);
  mx_irq_intc(MX_INTC_VU1, 0, 0);
  u32 st = mx_vpu_stat();
  mx_printf("d-bit (DE1 off): VPU-STAT %08x, %u interrupts\n", st, irqs);
  CHECK_EQ(st & (MX_VPU_VBS1 | 0x200), 0); /* ended, not stopped by D (VDS1) */
  CHECK_EQ(irqs, 0);
  return t_done("dbit");
}
