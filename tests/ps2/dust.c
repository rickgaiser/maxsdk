/* procgen's dust shader (demos/procgen/dustdraw.hlsl): single batches of every size
   through VIF1, read back from VU1 memory and compared with the same math
   on the EE: the two sprite corners, and the ADC bit of the second (a mote
   near the eye or reaching out of the guard band is not drawn). The motes
   are at random places of the box, the eye in its middle looking along +z,
   as procgen draws them. */

#include "dustdraw.h"
#include "t.h"

extern const u8 dustdraw_vu[], dustdraw_vu_end[];

#define N 128
#define BOX 48.0f
static float pos[N + 16][4] MX_ALIGNED(16);
static float uni[16][4] MX_ALIGNED(16);
static u64 chain[2 * 64] MX_ALIGNED(64);
static u32 out[1 + 3 * N][4] MX_ALIGNED(64);

static u32 seed = 7;
static float rnd01(void) {
  seed = seed * 1103515245u + 12345u;
  return (float)(s32)(seed >> 9 & 0xFFFF) / 65536.0f;
}
static float fmaxf_(float a, float b) { return a > b ? a : b; }
static float fabsf_(float a) { return a < 0 ? -a : a; }
static float floorf_(float v) {
  s32 i = (s32)v;
  return (float)i > v ? (float)(i - 1) : (float)i;
}

static int run(const struct mx_shader *s, u32 n, u32 *top) {
  static u32 mark = 0x300;
  mark = (mark + 1) & 0xFFFF;
  const void *streams[1] = {pos};
  u64 *c = chain;
  c = mx_e_cnt(c, 1, mx_vif_base(0), mx_vif_offset(s->buf_qw));
  c = mx_e_vif4(c, mx_vif_stcycl(1, 1), 0, 0, 0);
  c = mx_shader_e_batch(c, s, mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 0, 1, 1, 0, 0, 0, 0), uni, streams,
                        n);
  c = mx_e_end(c, 0, mx_vif_flush(), mx_vif_mark(mark));
  mx_dcache_writeback(chain, sizeof chain);
  mx_dcache_writeback(uni, sizeof uni);
  mx_dma_chain(MX_CH_VIF1, chain, 1);
  u32 t0 = mx_count();
  while ((MX_R32(MX_VIF_VIF1_MARK) & 0xFFFF) != mark)
    if (mx_count() - t0 > 100000000u)
      return -1;
  mx_dma_wait(MX_CH_VIF1);
  *top = MX_R32(MX_VIF_VIF1_TOP) & 0x3FF;
  return 0;
}

static int bad;
static void near(const char *what, u32 n, u32 i, s32 got, s32 want, s32 tol) {
  s32 d = got - want;
  if ((d > tol || -d > tol) && bad++ < 1)
    mx_printf("dust n %u mote %u: %s = %d, expected %d\n", n, i, what, got, want);
}

int main(void) {
  mx_gs_init(640, 448);
  mx_vif_reset(1);
  struct mx_shader s = MX_VU_SHADER(DUSTDRAW, dustdraw_vu, dustdraw_vu_end);
  for (u32 i = 0; i < N + 16; i++)
    pos[i][0] = rnd01(), pos[i][1] = rnd01(), pos[i][2] = rnd01(), pos[i][3] = rnd01() * 6;
  mx_dcache_writeback(pos, sizeof pos);
  /* the eye at (1000.3, 37.7, 2000.6) looking along +z; the box laid around
     it; procgen's mvp: X = 2048 w + FX x, Y = 2048 w - FY y, w = z */
  const float ex_ = 1000.3f, ey = 37.7f, ez = 2000.6f, FX = 457, FY = 457 * 0.467f * 2;
  float o[3] = {ex_ / BOX - 0.5f, ey / BOX - 0.5f, ez / BOX - 0.5f};
  float cx = o[0] * BOX - ex_, cy = -ey, cz = o[2] * BOX - ez; /* the corner from the eye */
  for (int i = 0; i < 16; i++)
    for (int k = 0; k < 4; k++)
      uni[i][k] = 0;
  /* p = (2048 w + FX x', 2048 w - FY y', z', w) for x' = x + cx etc., w = z' */
  uni[0][0] = FX, uni[1][1] = -FY, uni[2][0] = 2048, uni[2][1] = 2048, uni[2][2] = 1;
  uni[2][3] = 1;
  uni[3][0] = FX * cx + 2048 * cz, uni[3][1] = -FY * cy + 2048 * cz, uni[3][2] = cz, uni[3][3] = cz;
  uni[4][0] = -10, uni[4][1] = 400;                                   /* fog */
  uni[5][0] = o[0], uni[5][1] = o[1], uni[5][2] = o[2], uni[5][3] = BOX; /* box */
  uni[6][0] = o[1] * BOX, uni[6][1] = 0.035f * FX, uni[6][2] = 0.035f * FY;
  uni[7][0] = 255, uni[7][1] = 238, uni[7][2] = 200, uni[7][3] = 96;
  uni[8][0] = 2048, uni[8][1] = 1950, uni[8][2] = 1.2f;
  mx_vu_reset(2);
  mx_vif_reset(1);
  mx_vu_load(1, s.code, s.code_bytes, s.entry);
  u32 drawn = 0, total = 0;
  for (u32 n = 1; n <= s.max_n; n++) {
    u32 top;
    if (run(&s, n, &top)) {
      mx_printf("dust n %u: the batch did not end\n", n);
      t_fail++;
      break;
    }
    mx_vu1_read_dma(out, top + VU_DUSTDRAW_OUT_AT, 1 + 3 * n, (void *)0x70000000, 16384);
    CHECK_EQ(out[0][0] & 0x7FFF, n);
    bad = 0;
    for (u32 i = 0; i < n; i++) {
      float q[3];
      for (int k = 0; k < 3; k++) {
        q[k] = pos[i][k] - o[k];
        q[k] = (q[k] - floorf_(q[k])) * BOX;
      }
      q[1] += o[1] * BOX; /* ext.x: the box corner's height */
      float p[4] = {FX * q[0] + 2048 * q[2] + uni[3][0], -FY * q[1] + 2048 * q[2] + uni[3][1],
                    q[2] + cz, q[2] + cz};
      float r = 1 / p[3], X = p[0] * r, Y = p[1] * r, hw = r * uni[6][1], hh = r * uni[6][2];
      float e = fmaxf_(fmaxf_(fabsf_(X - 2048) + hw, fabsf_(Y - 2048) + hh) - 1950, 1.2f - p[3]);
      int adc = (out[3 + 3 * i][3] >> 15) & 1;
      if (fabsf_(e) > 0.01f) {
        near("adc", n, i, adc, e > 0, 0);
        total++;
        drawn += !adc;
      }
      if (!(e > 0)) {
        near("x0", n, i, (s32)out[2 + 3 * i][0], (s32)((X - hw) * 16), 4);
        near("y0", n, i, (s32)out[2 + 3 * i][1], (s32)((Y - hh) * 16), 4);
        near("x1", n, i, (s32)out[3 + 3 * i][0], (s32)((X + hw) * 16), 4);
      }
    }
    if (bad)
      t_fail++;
  }
  mx_printf("dust: %u of %u motes drawn\n", drawn, total);
  return t_done("dust");
}
