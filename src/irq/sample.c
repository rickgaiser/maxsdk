/* Timer-sampled unit state for mx_trace (docs/measuring.md). */

#include <mx/dma.h>
#include <mx/timer.h>
#include <mx/trace.h>
#include <mx/vu.h>

static volatile u32 samples, busy[MX_TS_N];

u32 mx_trace_units(void) {
  static const u8 ch[5] = {MX_CH_VIF0, MX_CH_VIF1, MX_CH_GIF, MX_CH_FROMSPR, MX_CH_TOSPR};
  u32 s = 0;
  for (int i = 0; i < 5; i++)
    if (mx_dma_busy(ch[i]))
      s |= 1u << i;
  /* an MFIFO drain keeps STR set while it waits on the empty ring: busy
     only while the ring holds data */
  u32 mfd = (u32)MX_GET(D_CTRL_MFD, MX_R32(MX_D_CTRL));
  if (mfd >= 2) {
    int drain = mfd == 2 ? MX_CH_VIF1 : MX_CH_GIF;
    if (MX_DMA_REG(drain, TADR) == MX_DMA_REG(MX_CH_FROMSPR, MADR))
      s &= ~(1u << (drain == MX_CH_VIF1 ? 1 : 2));
  }
  u32 vpu = mx_vpu_stat();
  if (vpu & MX_VPU_VBS0)
    s |= MX_TS_VU0;
  if (vpu & MX_VPU_VBS1)
    s |= MX_TS_VU1;
  if (vpu & MX_VPU_VGW1)
    s |= MX_TS_VU1GW;
  if (MX_GET(GIF_STAT_APATH, MX_R32(MX_GIF_STAT)))
    s |= MX_TS_PATH;
  if (MX_GET(GS_CSR_FIFO, MX_R64(MX_GS_CSR)) != 1)
    s |= MX_TS_GSFIFO;
  return s;
}

static void sample(void *arg) {
  (void)arg;
  u32 s = mx_trace_units();
  samples++;
  for (int i = 0; i < MX_TS_N; i++)
    if (s & (1u << i))
      busy[i]++;
  mx_trace(MX_TR_SAMPLE, (u16)s);
}

void mx_trace_sample(u32 hz) {
  if (!hz) {
    mx_timer_periodic(1, 0, 0, 0);
    return;
  }
  samples = 0;
  for (int i = 0; i < MX_TS_N; i++)
    busy[i] = 0;
  u32 period = (MX_BUS_HZ / 256) / hz;
  mx_timer_periodic(1, period ? period : 1, sample, 0);
}

u32 mx_trace_busy(u32 *out) {
  int was = mx_di();
  u32 n = samples;
  for (int i = 0; i < MX_TS_N; i++)
    out[i] = busy[i];
  mx_irq_restore(was);
  return n;
}
