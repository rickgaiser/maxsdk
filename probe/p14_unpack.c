/* UNPACK cost with skipping write (PS2 only, demos/vumax): VIF1 normal-mode
   DMA of 1024 vectors from memory in UNPACKs of 256, STCYCL CL = 1..4 with
   WL = 1 (every CL-th quadword written: the slots of in-place vertices), per
   format; then the same with WL = CL (filling, no skip) for comparison, and
   two streams into one vertex (pos + colour) as vumax sends them. EE cycles
   per vector, median of 8; VU1 idle. */

#include <mx/mx.h>

#define REPS 8
#define NV 1024

static u8 data[NV * 16] MX_ALIGNED(64);
static u64 chain[2 * 64] MX_ALIGNED(64);

static u32 median(u32 *v, int n) {
  for (int i = 1; i < n; i++)
    for (int j = i; j > 0 && v[j] < v[j - 1]; j--) {
      u32 t = v[j];
      v[j] = v[j - 1];
      v[j - 1] = t;
    }
  return v[n / 2];
}

struct fmt {
  const char *name;
  u32 fmt, bytes;
};
static const struct fmt fmts[] = {{"V3-32", MX_UNPACK_V3_32, 12}, {"V3-16", MX_UNPACK_V3_16, 6},
                                  {"V2-32", MX_UNPACK_V2_32, 8},  {"V4-8", MX_UNPACK_V4_8, 4},
                                  {"V4-32", MX_UNPACK_V4_32, 16}};

/* nv vectors of each of the streams, STCYCL cl, wl, into VU1 memory from 0 */
static u32 run(const struct fmt *const *f, u32 nf, u32 cl, u32 wl) {
  u32 v[REPS];
  u32 nv = NV / cl & ~7u; /* stay in VU1 memory; whole quadwords of every format */
  for (int r = 0; r < REPS; r++) {
    u64 *c = chain;
    c = mx_e_cnt(c, 0, mx_vif_stcycl(cl, wl), mx_vif_stmod(0));
    for (u32 s = 0; s < nf; s++)
      for (u32 i = 0; i < nv; i += 256) {
        u32 m = nv - i < 256 ? nv - i : 256;
        c = mx_e_ref(c, data + i * f[s]->bytes, m * f[s]->bytes / 16, 0,
                     mx_vif_unpack(f[s]->fmt, i * cl / wl + s, m, 0));
      }
    c = mx_e_end(c, 0, mx_vif_stcycl(1, 1), 0);
    mx_dcache_writeback(chain, sizeof chain);
    u32 t0 = mx_count();
    mx_dma_chain(MX_CH_VIF1, chain, 1);
    mx_dma_wait(MX_CH_VIF1);
    v[r] = (mx_count() - t0) * 100 / nv;
  }
  return median(v, REPS);
}

int main(void) {
  mx_vif_reset(1);
  for (u32 i = 0; i < sizeof data; i++)
    data[i] = (u8)i;
  mx_dcache_writeback(data, sizeof data);
  mx_printf("p14_unpack: EE cycles per vector (per vertex for two streams), VU1 idle\n");
  for (u32 f = 0; f < sizeof fmts / sizeof *fmts; f++) {
    const struct fmt *ff[1] = {&fmts[f]};
    mx_printf("p14 %-6s", fmts[f].name);
    for (u32 cl = 1; cl <= 4; cl++) {
      u32 c = run(ff, 1, cl, 1);
      mx_printf("  CL %u WL 1: %u.%02u", cl, c / 100, c % 100);
    }
    u32 c = run(ff, 1, 2, 2);
    mx_printf("  CL 2 WL 2: %u.%02u\n", c / 100, c % 100);
  }
  /* vumax's S2 batch: V3-16 / V3-32 positions + V4-8 colours, stride 2 */
  for (int p = 0; p < 2; p++) {
    const struct fmt *ff[2] = {&fmts[p], &fmts[3]};
    u32 c = run(ff, 2, 2, 1);
    mx_printf("p14 %s + V4-8, CL 2 WL 1: %u.%02u per vertex\n", fmts[p].name, c / 100, c % 100);
  }
  /* S3: positions, ST, colours, stride 3 */
  for (int p = 0; p < 2; p++) {
    const struct fmt *ff[3] = {&fmts[p], &fmts[2], &fmts[3]};
    u32 c = run(ff, 3, 3, 1);
    mx_printf("p14 %s + V2-32 + V4-8, CL 3 WL 1: %u.%02u per vertex\n", fmts[p].name, c / 100,
              c % 100);
  }
  MX_TEST_END(0);
  return 0;
}
