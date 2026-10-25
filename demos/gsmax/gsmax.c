/* gsmax: the GS at the rates of GS User's Manual 1.3 "Maximum Performance".

   Setup rate: every row of the manual's tables (points, lines, triangles,
   sprites; texture, shading, fog, antialiasing), with primitives of a few
   pixels so the pixels cost nothing, and with the fewest registers per
   primitive: the GIF hands the GS one 64-bit register per GS cycle, so a
   primitive that needs more registers than setup cycles is transfer-bound
   (a Gouraud point: RGBAQ + XYZ2 = 2 cycles, setup 1). The table shows both
   bounds; the floor is the larger one.

   Fill rate: 16 pixels per GS cycle untextured, 8 textured (32-bit pixels,
   Z and alpha blending on), with large sprites over the whole screen: one
   sprite, or tiles of one GS page (64 x 32 pixels, graphtip "Page Break
   Penalty"), or columns 32 pixels wide; with and without Z and alpha.

   The data comes over PATH1, the only path at the GS's register rate
   (docs/gif-gs.md: 2.0 EE cycles per register): a packet in VU1 memory,
   kicked K times by probe/kickn.vsm; (K kicks - 1 kick) / (K - 1) per packet,
   from MSCAL to the FINISH interrupt (probe/p9_gs.c). EE cycles; the GS
   runs at half the EE clock. */

#include <mx/mx.h>

extern const u8 kickn_vu[], kickn_vu_end[];

#define REPS 3
#define K 9
#define MAXQW 960 /* the packet in VU1 memory, from address 0 */

static u64 pk[2 * 1024] MX_ALIGNED(64);
static u64 env[256] MX_ALIGNED(64);
static u64 vp[64] MX_ALIGNED(64);
static u32 tex[64 * 64] MX_ALIGNED(64);

/* the registers of a vertex */
enum { R_XYZ = 1, R_RGBA = 2, R_ST = 4, R_UV = 8, R_FOG = 16 };

struct sc {
  const char *name;
  u32 prim;            /* MX_GS_PRIM_* */
  int iip, tme, fge, aa1;
  u32 regs;            /* R_* per vertex */
  u32 manual;          /* setup cycles per primitive (GS), 0: a fill case */
  u32 w, h;            /* primitive size in pixels (fill: sprite size) */
  int z, alpha;        /* Z test + write, alpha blending */
};

static const struct sc sc[] = {
    /* triangles, strips of 2-pixel triangles (one vertex per triangle) */
    {"tri flat", MX_GS_PRIM_TRISTRIP, 0, 0, 0, 0, R_XYZ, 2, 2, 2, 1, 0},
    {"tri flat fog", MX_GS_PRIM_TRISTRIP, 0, 0, 1, 0, R_FOG, 2, 2, 2, 1, 0},
    {"tri gouraud", MX_GS_PRIM_TRISTRIP, 1, 0, 0, 0, R_RGBA | R_XYZ, 4, 2, 2, 1, 0},
    {"tri gouraud fog", MX_GS_PRIM_TRISTRIP, 1, 0, 1, 0, R_RGBA | R_FOG, 5, 2, 2, 1, 0},
    {"tri tex flat", MX_GS_PRIM_TRISTRIP, 0, 1, 0, 0, R_ST | R_XYZ, 4, 2, 2, 1, 0},
    {"tri tex gouraud", MX_GS_PRIM_TRISTRIP, 1, 1, 0, 0, R_ST | R_RGBA | R_XYZ, 4, 2, 2, 1, 0},
    {"tri tex fog", MX_GS_PRIM_TRISTRIP, 0, 1, 1, 0, R_ST | R_FOG, 5, 2, 2, 1, 0},
    {"tri tex gour fog", MX_GS_PRIM_TRISTRIP, 1, 1, 1, 0, R_ST | R_RGBA | R_FOG, 5, 2, 2, 1, 0},
    {"tri flat AA1", MX_GS_PRIM_TRISTRIP, 0, 0, 0, 1, R_XYZ, 6, 2, 2, 1, 1},
    {"tri gouraud AA1", MX_GS_PRIM_TRISTRIP, 1, 0, 0, 1, R_RGBA | R_XYZ, 8, 2, 2, 1, 1},
    {"tri gour fog AA1", MX_GS_PRIM_TRISTRIP, 1, 0, 1, 1, R_RGBA | R_FOG, 9, 2, 2, 1, 1},
    {"tri tex AA1", MX_GS_PRIM_TRISTRIP, 0, 1, 0, 1, R_ST | R_XYZ, 8, 2, 2, 1, 1},
    {"tri tex fog AA1", MX_GS_PRIM_TRISTRIP, 0, 1, 1, 1, R_ST | R_FOG, 9, 2, 2, 1, 1},
    /* lines: strips of 2-pixel segments */
    {"line tex", MX_GS_PRIM_LINESTRIP, 0, 1, 0, 0, R_ST | R_XYZ, 4, 2, 2, 1, 0},
    {"line tex gouraud", MX_GS_PRIM_LINESTRIP, 1, 1, 0, 0, R_ST | R_RGBA | R_XYZ, 4, 2, 2, 1, 0},
    {"line tex fog", MX_GS_PRIM_LINESTRIP, 0, 1, 1, 0, R_ST | R_FOG, 5, 2, 2, 1, 0},
    {"line flat AA1", MX_GS_PRIM_LINESTRIP, 0, 0, 0, 1, R_XYZ, 6, 2, 2, 1, 1},
    {"line tex AA1", MX_GS_PRIM_LINESTRIP, 0, 1, 0, 1, R_ST | R_XYZ, 7, 2, 2, 1, 1},
    /* sprites of 2 x 2 pixels, two vertices each */
    {"sprite flat", MX_GS_PRIM_SPRITE, 0, 0, 0, 0, R_XYZ, 3, 2, 2, 1, 0},
    {"sprite tex", MX_GS_PRIM_SPRITE, 0, 1, 0, 0, R_UV | R_XYZ, 3, 2, 2, 1, 0},
    /* points */
    {"point flat", MX_GS_PRIM_POINT, 0, 0, 0, 0, R_XYZ, 1, 1, 1, 1, 0},
    {"point gouraud", MX_GS_PRIM_POINT, 1, 0, 0, 0, R_RGBA | R_XYZ, 1, 1, 1, 1, 0},
    {"point tex", MX_GS_PRIM_POINT, 0, 1, 0, 0, R_UV | R_XYZ, 1, 1, 1, 1, 0},
    /* fill: sprites over the whole screen */
    {"fill 640x224", MX_GS_PRIM_SPRITE, 0, 0, 0, 0, R_XYZ, 0, 640, 224, 1, 1},
    {"fill pages 64x32", MX_GS_PRIM_SPRITE, 0, 0, 0, 0, R_XYZ, 0, 64, 32, 1, 1},
    {"fill cols 32", MX_GS_PRIM_SPRITE, 0, 0, 0, 0, R_XYZ, 0, 32, 224, 1, 1},
    {"fill pages, no Z/a", MX_GS_PRIM_SPRITE, 0, 0, 0, 0, R_XYZ, 0, 64, 32, 0, 0},
    {"fill tex 640x224", MX_GS_PRIM_SPRITE, 0, 1, 0, 0, R_UV | R_XYZ, 0, 640, 224, 1, 1},
    {"fill tex pages", MX_GS_PRIM_SPRITE, 0, 1, 0, 0, R_UV | R_XYZ, 0, 64, 32, 1, 1},
    {"fill tex cols 32", MX_GS_PRIM_SPRITE, 0, 1, 0, 0, R_UV | R_XYZ, 0, 32, 224, 1, 1},
    {"fill tex pages no Z/a", MX_GS_PRIM_SPRITE, 0, 1, 0, 0, R_UV | R_XYZ, 0, 64, 32, 0, 0},
};
#define NSC (sizeof sc / sizeof *sc)

static u32 nregs(u32 r) { return (u32)__builtin_popcount(r); }

/* The register list of a vertex, in order: ST/UV, RGBAQ, XYZ2/XYZF2. */
static u64 reglist(u32 r) {
  u64 l = 0;
  u32 n = 0;
  if (r & R_ST)
    l |= (u64)MX_GIF_REG_ST << (4 * n++);
  if (r & R_UV)
    l |= (u64)MX_GIF_REG_UV << (4 * n++);
  if (r & R_RGBA)
    l |= (u64)MX_GIF_REG_RGBAQ << (4 * n++);
  l |= (u64)(r & R_FOG ? MX_GIF_REG_XYZF2 : MX_GIF_REG_XYZ2) << (4 * n++);
  return l;
}

/* One vertex at pixel (x, y), as REGLIST words. */
static u64 *vert(u64 *c, u32 r, u32 x, u32 y, u32 z, u32 i) {
  if (r & R_ST)
    *c++ = mx_gs_st((float)(s32)(x & 63) / 64.0f, (float)(s32)(y & 63) / 64.0f);
  if (r & R_UV)
    *c++ = mx_gs_uv((x & 1023) << 4, (y & 1023) << 4);
  if (r & R_RGBA)
    *c++ = mx_gs_rgbaq((i * 37) & 0xFF, (i * 91) & 0xFF, 0x80, 0x80, 1.0f);
  u32 px = mx_gs_px(x), py = mx_gs_py(y);
  if (r & R_FOG)
    *c++ = (u64)px | (u64)py << 16 | (u64)(z & 0xFFFFFF) << 32 | (u64)((i * 13) & 0xFF) << 56;
  else
    *c++ = mx_gs_xyz(px, py, z);
  return c;
}

/* The packet of scenario s: PRIM by A+D (REGLIST ignores the tag's PRIM),
   then one REGLIST loop per row of primitives. Returns the primitive count
   and sets *qwc; at most MAXQW quadwords. */
static u32 packet(const struct sc *s, u32 *qwc) {
  u64 *c = pk;
  u32 nr = nregs(s->regs), nprim = 0, z = 0x1000;
  c = mx_e_ad_tag(c, 1, 0);
  c = mx_e_ad(c, MX_GS_REG_PRIM,
              mx_gs_prim(s->prim, s->iip, s->tme, s->fge, s->alpha, s->aa1,
                         (s->regs & R_UV) != 0, 0, 0));
  if (!s->manual) {
    /* fill: sprites covering the 640 x 224 screen once */
    u32 n = (640 / s->w) * (224 / s->h);
    c = mx_e_giftag(c, 2 * n, 1, -1, MX_GIF_FLG_REGLIST, nr, reglist(s->regs));
    u64 *v0 = c;
    u32 i = 0;
    for (u32 y = 0; y + s->h <= 224; y += s->h)
      for (u32 x = 0; x + s->w <= 640; x += s->w) {
        c = vert(c, s->regs, x, y, z, i);
        c = vert(c, s->regs, x + s->w, y + s->h, z, i++);
      }
    if ((c - v0) & 1)
      *c++ = 0;
    *qwc = (u32)(c - pk) / 2;
    return n;
  }
  /* rows of primitives across the screen, each row one REGLIST loop; as many
     rows as fit the packet */
  u64 *last = 0;
  u32 per_row = s->prim == MX_GS_PRIM_POINT ? 640 : s->prim == MX_GS_PRIM_SPRITE ? 320 : 638;
  /* a row fits the packet (one tag) */
  u32 nv_max = 2 * (MAXQW - 4) / nr;
  if (s->prim == MX_GS_PRIM_SPRITE && per_row > nv_max / 2)
    per_row = nv_max / 2;
  else if (s->prim != MX_GS_PRIM_SPRITE && per_row > nv_max - 2)
    per_row = nv_max - 2;
  for (u32 row = 0;; row++) {
    u32 nv = s->prim == MX_GS_PRIM_TRISTRIP  ? per_row + 2
             : s->prim == MX_GS_PRIM_LINESTRIP ? per_row + 1
             : s->prim == MX_GS_PRIM_SPRITE    ? 2 * per_row
                                               : per_row;
    u32 need = (u32)(c - pk) / 2 + 1 + (nv * nr + 1) / 2;
    if (need > MAXQW)
      break;
    u32 y = (row * 3) % 220;
    z++;
    last = c;
    c = mx_e_giftag(c, nv, 0, -1, MX_GIF_FLG_REGLIST, nr, reglist(s->regs));
    u64 *v0 = c;
    for (u32 j = 0; j < nv; j++) {
      u32 x, yy;
      if (s->prim == MX_GS_PRIM_TRISTRIP)
        x = j, yy = y + (j & 1) * 2;
      else if (s->prim == MX_GS_PRIM_LINESTRIP)
        x = 2 * (j / 2) + (j & 1), yy = y + (j & 1) * 2;
      else if (s->prim == MX_GS_PRIM_SPRITE)
        x = 2 * (j / 2) + (j & 1) * 2, yy = y + (j & 1) * 2;
      else
        x = j, yy = y;
      c = vert(c, s->regs, x, yy, z, j);
    }
    if ((c - v0) & 1)
      *c++ = 0;
    nprim += per_row;
  }
  /* the last tag ends the packet */
  last[0] |= MX_SET(GIFTAG_EOP, 1);
  *qwc = (u32)(c - pk) / 2;
  return nprim;
}

/* Clear; Z test GEQUAL with writes (or ALWAYS, masked), alpha blending, texture, fog
   colour; wait until the GS has done it. */
static void setup(const struct sc *s) {
  u64 *c = env, *t = c;
  c = mx_e_cnt(c, 0, 0, 0);
  c = mx_gs_e_draw_env(c, 0, 0);
  c = mx_gs_e_clear(c, 0, 0, 0, 0);
  c = mx_e_ad_tag(c, 9, 1);
  c = mx_e_ad(c, MX_GS_REG_TEST_1, mx_gs_test(0, 0, 0, 0, 0, 0, 1, s->z ? 2 : 1));
  c = mx_e_ad(c, MX_GS_REG_ZBUF_1, mx_gs_zbuf(mx_gs.zbp, MX_GS_PSM_Z24, !s->z));
  /* Cs * As + Cd * (1 - As) */
  c = mx_e_ad(c, MX_GS_REG_ALPHA_1, mx_gs_alpha(0, 1, 0, 1, 0x80));
  c = mx_e_ad(c, MX_GS_REG_TEX0_1, mx_gs_tex0(mx_gs.tbp, 1, MX_GS_PSM_CT32, 6, 6, 1, 0));
  c = mx_e_ad(c, MX_GS_REG_TEX1_1, mx_gs_tex1(0, 0, 0, 0, 0, 0, 0));
  c = mx_e_ad(c, MX_GS_REG_CLAMP_1, mx_gs_clamp(0, 0, 0, 0, 0, 0));
  c = mx_e_ad(c, MX_GS_REG_FOGCOL, 0x406080);
  c = mx_e_ad(c, MX_GS_REG_RGBAQ, mx_gs_rgbaq(0x80, 0x60, 0x40, 0x80, 1.0f));
  c = mx_e_ad(c, MX_GS_REG_FINISH, 0);
  mx_e_close(t, c);
  c = mx_e_end(c, 0, 0, 0);
  mx_dcache_writeback(env, sizeof env);
  u32 f0 = mx_gs.finish;
  mx_gs_send(env);
  mx_gs_wait_finish(f0, 1);
}

/* Kick the packet at VU1 address 0 n times; until the GS has drawn it all. */
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
    if (mx_count() - t0 > 300000000u)
      return 0;
  mx_dma_chain(MX_CH_GIF, env, 0);
  mx_gs_wait_finish(f0, 1);
  return mx_count() - t0;
}

static u32 median(u32 *v, int n) {
  for (int i = 1; i < n; i++)
    for (int j = i; j > 0 && v[j] < v[j - 1]; j--) {
      u32 t = v[j];
      v[j] = v[j - 1];
      v[j - 1] = t;
    }
  return v[n / 2];
}

static int timeout;

/* EE cycles per packet of scenario s (at least 1) */
static u32 run(const struct sc *s, u32 *nprim, u32 *qwc) {
  *nprim = packet(s, qwc);
  mx_dcache_writeback(pk, *qwc * 16);
  u64 *u = vp;
  for (u32 a = 0; a < *qwc; a += 128) {
    u32 m = *qwc - a < 128 ? *qwc - a : 128;
    u = mx_e_ref(u, pk + 2 * a, m, mx_vif_stcycl(1, 1), mx_vif_unpack(MX_UNPACK_V4_32, a, m, 0));
  }
  u = mx_e_end(u, 0, 0, 0);
  mx_dcache_writeback(vp, sizeof vp);
  mx_dma_chain(MX_CH_VIF1, vp, 1);
  mx_dma_wait(MX_CH_VIF1);
  u32 v[REPS];
  for (int r = 0; r < REPS; r++) {
    setup(s);
    u32 t1 = kick(1);
    setup(s);
    u32 tk = kick(K);
    timeout |= !t1 || !tk;
    /* (PCSX2 draws K kicks as fast as one) */
    v[r] = tk > t1 + K ? (tk - t1) / (K - 1) : 1;
  }
  return median(v, REPS);
}

/* x / y with two decimals */
static void print2(u32 x, u32 y) {
  u32 c = (u32)((u64)x * 100 / y);
  mx_printf("%u.%02u", c / 100, c % 100);
}

int main(void) {
  mx_gs_init(640, 224);
  mx_vif_reset(1);
  mx_vu_load(1, kickn_vu, (u32)(kickn_vu_end - kickn_vu), 0);
  for (u32 i = 0; i < 64 * 64; i++)
    tex[i] = 0x80000000u | (((i ^ i >> 6) & 8) ? 0xC0C0C0u : 0x606060u);
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
  mx_printf("gsmax: PATH1, %u kicks of a packet in VU1 memory; EE cycles (GS = EE / 2)\n", K);
  mx_printf("gsmax | case | regs/prim | manual GS cycles | floor GS cycles | measured GS cycles "
            "| M prims/s | of floor |\n");
  mx_debug_hold(1);
  int fail = 0;
  for (u32 i = 0; i < NSC; i++) {
    const struct sc *s = &sc[i];
    u32 n, q, cyc = run(s, &n, &q);
    if (timeout) {
      mx_printf("gsmax %s: the GS did not finish\n", s->name);
      fail = 1;
      break;
    }
    if (s->manual) {
      /* registers per primitive: strips one vertex, sprites two */
      u32 rp = nregs(s->regs) * (s->prim == MX_GS_PRIM_SPRITE ? 2 : 1);
      u32 floor = rp > s->manual ? rp : s->manual;
      mx_printf("gsmax | %-18s | %u | %u | %u | ", s->name, rp, s->manual, floor);
      print2(cyc, 2 * n); /* GS cycles per primitive */
      mx_printf(" | ");
      print2(n * (MX_EE_HZ / 10000), cyc * 100); /* M prims/s */
      mx_printf(" | %u%% |\n", (u32)((u64)floor * 2 * n * 100 / cyc));
    } else {
      /* pixels per GS cycle */
      u32 px = (640 / s->w) * s->w * (224 / s->h) * s->h, want = s->tme ? 8 : 16;
      mx_printf("gsmax | %-18s | %u sprites | %u px/cycle | | ", s->name, n, want);
      print2(2 * px, cyc);
      mx_printf(" px/cycle | ");
      print2((u32)((u64)px * (MX_EE_HZ / 10000) / cyc), 100000); /* G px/s */
      mx_printf(" Gpx/s | %u%% |\n", (u32)((u64)px * 2 * 100 / cyc / want));
    }
    mx_debug_flush();
  }
  mx_debug_hold(0);
  if (mx_gs_save_tga(0, "host:gsmax.tga") == 0)
    mx_printf("gsmax: the last case saved to gsmax.tga\n");
  MX_TEST_END(fail);
  return fail;
}
