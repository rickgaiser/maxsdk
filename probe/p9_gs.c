/* PLAN_PROCGEN P0 (PS2 only): what the GS costs per triangle.

   PATH3: one GIF packet of NTRI triangles of side * side / 2 pixels each, in
   rows across the screen (wrapping to the top), from the DMA start to the
   FINISH interrupt. Varied: shading (flat / Gouraud / textured), lists vs
   strips, PACKED vs REGLIST, point vs bilinear filtering, and vertices with
   ADC set (sent, never drawn: the transfer alone).

   PATH1: VU1 kicks the same strip packet (P1TRI triangles, in VU1 memory)
   K times; (time for K - time for 1) / (K - 1) per packet, so the start
   cost cancels. Z test GEQUAL with Z writes, as in a scene. */

#include <mx/mx.h>

#define REPS 3
#define NTRI 4096
#define P1TRI 256
#define K 9

extern const u8 kickn_vu[], kickn_vu_end[];

enum { FLAT, GOUR, TEX };
static const char *const fmt_name[] = {"flat", "gouraud", "textured"};

static u64 pk[96 * 1024] MX_ALIGNED(64); /* 48 k qw */
static u64 env[128] MX_ALIGNED(64);
static u64 vp[64] MX_ALIGNED(64);
static u32 tex[64 * 64] MX_ALIGNED(64);

struct cfg {
  int fmt, reglist, strip, side, adc, bilinear;
};

static u32 nregs(int fmt) { return fmt == FLAT ? 1 : fmt == GOUR ? 2 : 3; }

static u32 median(u32 *v, int n) {
  for (int i = 1; i < n; i++)
    for (int j = i; j > 0 && v[j] < v[j - 1]; j--) {
      u32 t = v[j];
      v[j] = v[j - 1];
      v[j - 1] = t;
    }
  return v[n / 2];
}

/* One vertex: 64-bit register words (REGLIST) or quadwords (PACKED). */
static u64 *vert(u64 *c, const struct cfg *k, u32 x, u32 y, u32 z, u32 i) {
  u32 px = mx_gs_px(x), py = mx_gs_py(y);
  u32 r = (i * 37) & 0xFF, g = (i * 91) & 0xFF, b = 0x80;
  float s = (float)(s32)(x & 63) / 64.0f, t = (float)(s32)(y & 63) / 64.0f;
  if (k->reglist) {
    if (k->fmt == TEX)
      *c++ = mx_gs_st(s, t);
    if (k->fmt != FLAT)
      *c++ = mx_gs_rgbaq(r, g, b, 0x80, 1.0f);
    *c++ = mx_gs_xyz(px, py, z);
    return c;
  }
  if (k->fmt == TEX)
    c = mx_e_st(c, s, t, 1.0f);
  if (k->fmt != FLAT)
    c = mx_e_rgbaq(c, r, g, b, 0x80);
  return mx_e_xyz2(c, px, py, z, k->adc);
}

/* ntri triangles of k->side, one GIF packet per row of the screen. */
static u64 *tris(u64 *c, const struct cfg *k, u32 ntri, int eop) {
  u32 s = (u32)k->side, cols = mx_gs.w / s, rows = mx_gs.h / s;
  u32 xyz = k->reglist && k->adc ? MX_GIF_REG_XYZ3 : MX_GIF_REG_XYZ2;
  u64 regs = k->fmt == TEX    ? mx_gif_regs3(MX_GIF_REG_ST, MX_GIF_REG_RGBAQ, xyz)
             : k->fmt == GOUR ? mx_gif_regs2(MX_GIF_REG_RGBAQ, xyz)
                              : xyz;
  u32 prim = (u32)mx_gs_prim(k->strip ? MX_GS_PRIM_TRISTRIP : MX_GS_PRIM_TRIANGLE,
                             k->fmt != FLAT, k->fmt == TEX, 0, 0, 0, 0, 0, 0);
  u32 flg = k->reglist ? MX_GIF_FLG_REGLIST : MX_GIF_FLG_PACKED;
  for (u32 done = 0, row = 0; done < ntri; row++) {
    u32 n = ntri - done < 2 * cols ? ntri - done : 2 * cols;
    u32 nv = k->strip ? n + 2 : 3 * n;
    u32 y = (row % rows) * s, z = 0x1000 + row, i = 0;
    /* the GS ignores the tag's PRIM in REGLIST mode (PS2 and PCSX2: the
       previous PRIM, a sprite, stayed), so write PRIM by A+D first */
    if (k->reglist) {
      c = mx_e_ad_tag(c, 1, 0);
      c = mx_e_ad(c, MX_GS_REG_PRIM, prim);
    }
    c = mx_e_giftag(c, nv, eop && done + n == ntri, (s32)prim, flg, nregs(k->fmt), regs);
    u64 *v0 = c;
    if (k->strip)
      for (u32 j = 0; j < nv; j++)
        c = vert(c, k, (j / 2) * s, y + (j & 1) * s, z, i++);
    else
      for (u32 t = 0; t < n; t++) {
        u32 x = (t / 2) * s;
        if (t & 1) {
          c = vert(c, k, x + s, y, z, i++);
          c = vert(c, k, x + s, y + s, z, i++);
          c = vert(c, k, x, y + s, z, i++);
        } else {
          c = vert(c, k, x, y, z, i++);
          c = vert(c, k, x + s, y, z, i++);
          c = vert(c, k, x, y + s, z, i++);
        }
      }
    if ((c - v0) & 1)
      *c++ = 0; /* REGLIST: pad to a quadword */
    done += n;
  }
  return c;
}

/* Clear, then Z test GEQUAL, the texture and filter for k; wait for it. */
static void setup(const struct cfg *k) {
  u64 *c = env, *t = c;
  c = mx_e_cnt(c, 0, 0, 0);
  c = mx_gs_e_draw_env(c, 0, 0);
  c = mx_gs_e_clear(c, 0, 0, 0, 0);
  c = mx_e_ad_tag(c, 5, 1);
  c = mx_e_ad(c, MX_GS_REG_TEST_1, mx_gs_test(0, 0, 0, 0, 0, 0, 1, 2));
  c = mx_e_ad(c, MX_GS_REG_TEX0_1, mx_gs_tex0(mx_gs.tbp, 1, MX_GS_PSM_CT32, 6, 6, 1, 0));
  c = mx_e_ad(c, MX_GS_REG_TEX1_1, mx_gs_tex1(0, 0, k->bilinear, k->bilinear, 0, 0, 0));
  c = mx_e_ad(c, MX_GS_REG_RGBAQ, mx_gs_rgbaq(0x80, 0x60, 0x40, 0x80, 1.0f));
  c = mx_e_ad(c, MX_GS_REG_FINISH, 0);
  mx_e_close(t, c);
  c = mx_e_end(c, 0, 0, 0);
  mx_dcache_writeback(env, sizeof env);
  u32 f0 = mx_gs.finish;
  mx_gs_send(env);
  mx_gs_wait_finish(f0, 1);
}

static u32 run3(const struct cfg *k) {
  u64 *c = pk, *t = c;
  c = mx_e_cnt(c, 0, 0, 0);
  c = tris(c, k, NTRI, 0);
  c = mx_e_ad_tag(c, 1, 1);
  c = mx_e_ad(c, MX_GS_REG_FINISH, 0);
  mx_e_close(t, c);
  c = mx_e_end(c, 0, 0, 0);
  mx_dcache_writeback(pk, (u32)((u8 *)c - (u8 *)pk));
  u32 v[REPS];
  for (int r = 0; r < REPS; r++) {
    setup(k);
    u32 f0 = mx_gs.finish, t0 = mx_count();
    mx_dma_chain(MX_CH_GIF, pk, 0);
    mx_gs_wait_finish(f0, 1);
    v[r] = mx_count() - t0;
  }
  return median(v, REPS);
}

/* PATH1: the packet (qwc quadwords at pk) is in VU1 memory; kick it n times
   and wait until VU1 and PATH1 are done, then for the GS (FINISH). */
static u32 kick(u32 n) {
  static u32 mark = 0x100;
  u64 *c = vp;
  c = mx_e_cnt(c, 1, mx_vif_stcycl(1, 1), mx_vif_unpack(MX_UNPACK_V4_32, 1023, 1, 0));
  c = mx_e_w4(c, n, 0, 0, 0);
  mark = (mark + 1) & 0xFFFF;
  c = mx_e_end(c, 1, mx_vif_mscal(0), mx_vif_flush());
  c = mx_e_w4(c, mx_vif_mark(mark), 0, 0, 0);
  mx_dcache_writeback(vp, sizeof vp);
  u64 *f = env, *ft = f;
  f = mx_e_cnt(f, 0, 0, 0);
  f = mx_e_ad_tag(f, 1, 1);
  f = mx_e_ad(f, MX_GS_REG_FINISH, 0);
  mx_e_close(ft, f);
  f = mx_e_end(f, 0, 0, 0);
  mx_dcache_writeback(env, sizeof env);
  u32 f0 = mx_gs.finish, t0 = mx_count();
  mx_dma_chain(MX_CH_VIF1, vp, 1);
  while ((MX_R32(MX_VIF_VIF1_MARK) & 0xFFFF) != mark)
    ;
  mx_dma_chain(MX_CH_GIF, env, 0);
  mx_gs_wait_finish(f0, 1);
  return mx_count() - t0;
}

static u32 p1_t1, p1_tk;

static u32 run1(const struct cfg *k, u32 *qwc) {
  u64 *c = tris(pk, k, P1TRI, 1);
  u32 n = (u32)(c - pk) / 2;
  *qwc = n;
  mx_dcache_writeback(pk, n * 16);
  /* upload the packet to VU1 address 0 */
  u64 *u = vp;
  for (u32 a = 0; a < n; a += 128) {
    u32 m = n - a < 128 ? n - a : 128;
    u = mx_e_ref(u, pk + 2 * a, m, mx_vif_stcycl(1, 1), mx_vif_unpack(MX_UNPACK_V4_32, a, m, 0));
  }
  u = mx_e_end(u, 0, 0, 0);
  mx_dcache_writeback(vp, sizeof vp);
  mx_dma_chain(MX_CH_VIF1, vp, 1);
  mx_dma_wait(MX_CH_VIF1);
  u32 v[REPS];
  for (int r = 0; r < REPS; r++) {
    setup(k);
    u32 t1 = kick(1);
    setup(k);
    u32 tk = kick(K);
    v[r] = tk > t1 ? (tk - t1) / (K - 1) : 0;
    p1_t1 = t1, p1_tk = tk;
  }
  return median(v, REPS);
}

static void line(const char *path, const struct cfg *k, u32 ntri, u32 cyc, u32 qwc) {
  u32 area2 = (u32)(k->side * k->side); /* twice the area */
  mx_printf("%s %-8s %-5s %-7s %s%s side %2d (%3u px): %7u cyc, %3u.%02u cyc/tri, %u.%02u cyc/qw\n",
            path, fmt_name[k->fmt], k->strip ? "strip" : "list",
            k->reglist ? "reglist" : "packed", k->adc ? "adc " : "", k->bilinear ? "bilin " : "",
            k->side, area2 / 2, cyc, cyc / ntri, cyc * 100 / ntri % 100, cyc / qwc,
            cyc * 100 / qwc % 100);
  mx_debug_flush();
}

/* Pixels of the first 32 of line 1 that are not black (the triangles
   cover them unless ADC is set). */
static u32 drawn(void) {
  static u32 px[32] MX_ALIGNED(64);
  mx_gs_download(px, MX_GS_PAGE(mx_gs.fbp[0]), mx_gs.fbw, MX_GS_PSM_CT32, 0, 1, 32, 1);
  u32 n = 0;
  for (int i = 0; i < 32; i++)
    n += (px[i] & 0xFFFFFF) != 0;
  return n;
}

static u32 qw_of(const struct cfg *k, u32 ntri) {
  u64 *c = tris(pk, k, ntri, 1);
  return (u32)(c - pk) / 2;
}

int main(void) {
  mx_gs_init(640, 224);
  mx_vif_reset(1);
  mx_vu_load(1, kickn_vu, (u32)(kickn_vu_end - kickn_vu), 0);
  for (u32 i = 0; i < 64 * 64; i++)
    tex[i] = 0x80000000u | (i * 0x010305u & 0xFFFFFFu);
  mx_dcache_writeback(tex, sizeof tex);
  {
    struct mx_pkt p;
    static u32 up[256] MX_ALIGNED(64);
    mx_pkt_begin(&p, up, sizeof up, 0);
    mx_pkt_cnt(&p, 0, 0);
    mx_gs_upload(&p, mx_gs.tbp, 1, MX_GS_PSM_CT32, 0, 0, 64, 64, tex);
    mx_gs_finish_ad(&p);
    mx_pkt_end(&p, 0, 0);
    mx_pkt_finish(&p);
    mx_pkt_writeback(&p);
    u32 f0 = mx_gs.finish;
    mx_gs_send(up);
    mx_gs_wait_finish(f0, 1);
  }

  mx_printf("p9_gs: PATH3 %u triangles, PATH1 %u triangles x %u kicks, Z GEQUAL\n", NTRI, P1TRI,
            K);
  mx_debug_hold(1);
  static const int sides[] = {2, 4, 8, 16, 32};
  /* PATH3: every shading x topology x format x size */
  for (int f = FLAT; f <= TEX; f++)
    for (int st = 1; st >= 0; st--)
      for (int rl = 0; rl < 2; rl++) {
        struct cfg k = {f, rl, st, 4, 1, 0};
        line("p3", &k, NTRI, run3(&k), qw_of(&k, NTRI));
        mx_printf("   (%u of 32 pixels drawn)\n", drawn());
        for (u32 s = 0; s < sizeof sides / sizeof *sides; s++) {
          k.adc = 0, k.side = sides[s];
          line("p3", &k, NTRI, run3(&k), qw_of(&k, NTRI));
          if (!s)
            mx_printf("   (%u of 32 pixels drawn)\n", drawn());
        }
      }
  for (u32 s = 0; s < sizeof sides / sizeof *sides; s++) {
    struct cfg k = {TEX, 0, 1, sides[s], 0, 1};
    line("p3", &k, NTRI, run3(&k), qw_of(&k, NTRI));
  }
  /* PATH1: strips */
  for (int f = GOUR; f <= TEX; f++)
    for (int rl = 0; rl < 2; rl++) {
      struct cfg k = {f, rl, 1, 4, 1, 0};
      u32 q, cyc = run1(&k, &q);
      line("p1", &k, P1TRI, cyc, q);
      mx_printf("   (1 kick %u, %u kicks %u cycles; %u of 32 pixels drawn)\n", p1_t1, K, p1_tk,
                drawn());
      for (u32 s = 0; s < 4; s++) {
        k.adc = 0, k.side = sides[s];
        cyc = run1(&k, &q);
        line("p1", &k, P1TRI, cyc, q);
        if (!s)
          mx_printf("   (%u of 32 pixels drawn)\n", drawn());
      }
    }
  mx_debug_hold(0);
  MX_TEST_END(0);
  return 0;
}
