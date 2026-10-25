/* vumax: how many vertices VU1 can transform, for six standard vertex
   programs, with kernels at the hardware floor (tools/vusched/vusched.py
   writes them, vx_*.vsm) and the same programs in HLSL (vh_*.hlsl) next to
   them.

     S1 flat     MVP, perspective divide, XYZ2 (one colour per draw)
     S2 gouraud  + a colour per vertex
     S3 tex      + ST, perspective correct
     S4 lit      one directional light (no texture)
     S5 littex   one light, textured
     S6 scei     ps2gl's SCEI shader: three lights, clamped, textured, guard
                 band clipping of the strip's triangles by ADC
     S3b tex16   S3 with ST as V2-16 (4.12 fixed point, ITOF12): one upper op
                 more, a third less for the VIF, which is S3's limit

   The floor of each: the upper (FMAC) ops per vertex, 6 6 7 11 12 17 8 cycles;
   S1 and S2 would be 7 (DIV) but take every second 1/w from the EFU.

   In-place batches: the VIF unpacks each input into the quadword of the GIF
   register it becomes (STCYCL skipping write), so a colour goes from memory
   into the GIF packet without the VU; the kernel overwrites the position
   with XYZ2, the normal with RGBAQ, ST with ST * q. VU1 data memory:

     0, 16       two uniform contexts, by turns per draw
     32 + 330 b  three buffers b: pad, header (n, context, kick, loop end),
                 GIF tag, n vertices of `stride` quadwords, slop

   Three buffers: batch k + 3 is unpacked into batch k's buffer only after
   MSCAL k + 2 is taken, i.e. after program k + 1 passed its XGKICK, which
   waited until the GIF had sent packet k.

   Input formats: f32 (positions V3-32) and c16 (positions V3-16, made
   floats by the VIF: STMOD offset mode adds ROW = 1.5 * 2^23 to the integer,
   which is the float 1.5 * 2^23 + p; the MVP takes the offset out). Normals
   V3-32, ST V2-32 (z = 1.0 from the VIF's COL register by STMASK), colours
   V4-8.

   Parts: (1) every kernel, format and batch size against the same math on
   the EE; (2) 64 terrain patches of 33 x 33 vertices (levels.c's scene A)
   per frame without vsync, each kernel with and without XGKICK (without:
   VU1 and VIF only), then the HLSL twins; (3) a frame of S6 saved as
   vumax.tga. */

#include <mx/mx.h>
#include "vh_flat.h"
#include "vh_gour.h"
#include "vh_lit.h"
#include "vh_littex.h"
#include "vh_scei.h"
#include "vh_tex.h"
#include "vh_tex16.h"
#include "vx_kernels.h"

extern const u8 vh_flat_vu[], vh_flat_vu_end[], vh_gour_vu[], vh_gour_vu_end[], vh_tex_vu[],
    vh_tex_vu_end[], vh_lit_vu[], vh_lit_vu_end[], vh_littex_vu[], vh_littex_vu_end[],
    vh_scei_vu[], vh_scei_vu_end[], vh_tex16_vu[], vh_tex16_vu_end[];

#define CTX_QW 16
#define BUF_AT 32
#define BUF_QW 330
#define CODE_AT 1024 /* 64-bit units; the HLSL programs are below */
#define MAGIC 12582912.0f /* 1.5 * 2^23 */
#define MAGIC_BITS 0x4B400000u
#define GUARD 2000.0f /* S6: guard band, pixels from the centre */

#define G 33
#define NV (2 * G * (G - 1) + 2 * (G - 2)) /* strip vertices of a patch */
#define PATCHES 64
#define FRAMES 20
#define NT 320 /* test vertices */

enum { S_FLAT, S_GOUR, S_TEX, S_LIT, S_LITTEX, S_SCEI, S_TEX16, NS };
enum { F32, C16, NF };
static const char *const fmt_name[NF] = {"f32", "c16"};

struct vx {
  const char *name;
  const u8 *code, *end;
  u32 u, ii, stride, stages, pad, slop;
  u32 at;               /* micro address, 64-bit units */
  u32 max_n;            /* vertices per batch (max_n()) */
  s8 st, col, nrm, pos; /* the quadword of each input in a vertex, -1: none */
  u8 iip, tme;
};

#define VX_ROW(n, u_, ii_, stride_, stages_, pad_, slop_)                                          \
  {#n, n##_vu, n##_vu_end, u_, ii_, stride_, stages_, pad_, slop_, 0, 0, -1, -1, -1, -1, 1, 0},
static struct vx vx[NS] = {VX_KERNELS(VX_ROW)};

static void vx_slots(void) {
  static const s8 sl[NS][4] = {/* st col nrm pos */
                               {-1, -1, -1, 0}, {-1, 0, -1, 1}, {0, 1, -1, 2},
                               {-1, -1, 0, 1},  {0, -1, 1, 2},  {0, -1, 1, 2}, {0, 1, -1, 2}};
  u32 at = CODE_AT;
  for (int s = 0; s < NS; s++) {
    vx[s].st = sl[s][0], vx[s].col = sl[s][1], vx[s].nrm = sl[s][2], vx[s].pos = sl[s][3];
    vx[s].iip = s != S_FLAT;
    vx[s].tme = s == S_TEX || s == S_LITTEX || s == S_SCEI || s == S_TEX16;
    vx[s].at = at;
    at += (u32)(vx[s].end - vx[s].code) / 8;
  }
}

static u32 nreg(const struct vx *k) { return k->stride; }
static u64 regs(const struct vx *k) {
  return k->stride == 1   ? MX_GIF_REG_XYZ2
         : k->stride == 2 ? mx_gif_regs2(MX_GIF_REG_RGBAQ, MX_GIF_REG_XYZ2)
                          : mx_gif_regs3(MX_GIF_REG_ST, MX_GIF_REG_RGBAQ, MX_GIF_REG_XYZ2);
}
static u32 prim(const struct vx *k) {
  return (u32)mx_gs_prim(MX_GS_PRIM_TRISTRIP, k->iip, k->tme, 0, 0, 0, 0, 0, 0);
}

/* ---- buffers ---------------------------------------------------------------------- */

static u32 pre(const struct vx *k) { return k->pad > 2 ? k->pad - 2 : 0; }
static u32 hdr_at(const struct vx *k, u32 b) { return BUF_AT + b * BUF_QW + pre(k); }
/* kernel iterations of a batch of n: the vertices' and S - 1 more */
static u32 iters(const struct vx *k, u32 n) { return (n + k->u - 1) / k->u + k->stages - 1; }
static int fits(const struct vx *k, u32 n) {
  u32 room = BUF_QW - pre(k) - 2;
  return iters(k, n) * k->u * k->stride <= room && ((n + 7) & ~7u) * k->stride <= room;
}
static u32 max_n(const struct vx *k) {
  u32 n = 1;
  while (fits(k, n + 1))
    n++;
  return n;
}
static void vx_batches(void) {
  for (int s = 0; s < NS; s++)
    vx[s].max_n = max_n(&vx[s]);
}

/* ---- the streams -------------------------------------------------------------------- */

struct mesh {
  float (*pos32)[3];
  s16 (*pos16)[3];
  u32 *col;
  float (*nrm)[3];
  float (*st)[2];
  s16 (*st16)[2]; /* 4.12 fixed point (S3b) */
  float p16;      /* pos16 = p * p16 */
};

/* VIF state that the batches use: STMASK (the ST unpack: z from COL, w
   kept), ROW (the c16 offset), COL (1.0). */
static u64 *e_vif_setup(u64 *c, int st16) {
  union {
    float f;
    u32 u;
  } one = {1.0f};
  /* z of ST: 1.0, or 4096 for the 4.12 fixed point of S3b */
  if (st16)
    one.u = 4096;
  c = mx_e_cnt(c, 3, 0, 0);
  c = mx_e_w4(c, mx_vif_stmask(), 0xE0E0E0E0u, mx_vif_strow(), MAGIC_BITS);
  c = mx_e_w4(c, MAGIC_BITS, MAGIC_BITS, MAGIC_BITS, mx_vif_stcol());
  return mx_e_w4(c, one.u, one.u, one.u, one.u);
}
#define VIF_SETUP_QW 4

static u64 *e_context(u64 *c, u32 ctx, const u32 (*uni)[4], u32 nq) {
  c = mx_e_cnt(c, 0, mx_vif_stmod(0), mx_vif_stcycl(1, 1));
  c = mx_e_cnt(c, nq, 0, mx_vif_unpack(MX_UNPACK_V4_32, ctx, nq, 0));
  for (u32 i = 0; i < nq; i++)
    c = mx_e_w4(c, uni[i][0], uni[i][1], uni[i][2], uni[i][3]);
  return c;
}

/* nn vertices of one input (bytes each) into their slots from addr: UNPACKs
   of at most 256 (NUM has 8 bits). */
static u64 *e_stream(u64 *c, const struct vx *k, const void *data, u32 bytes, u32 nn, u32 mod,
                     u32 fmt, u32 addr, u32 flags) {
  const u8 *d = data;
  for (u32 i = 0; i < nn; i += 256) {
    u32 m = nn - i < 256 ? nn - i : 256;
    c = mx_e_ref(c, d + i * bytes, m * bytes / 16, mx_vif_stmod(mod),
                 mx_vif_unpack(fmt, addr + i * k->stride, m, flags));
  }
  return c;
}

/* One batch: vertices v .. v + n - 1 of m into buffer b. */
static u64 *e_batch(u64 *c, const struct vx *k, int fmt, const struct mesh *m, u32 b, u32 ctx,
                    u32 v, u32 n, int kick) {
  u32 B = hdr_at(k, b), V0 = B + 2, nn = (n + 7) & ~7u;
  /* the c16 position stream of the batch before left offset mode on */
  c = mx_e_cnt(c, 0, mx_vif_stmod(0), 0);
  c = mx_e_cnt(c, 3, mx_vif_stcycl(1, 1), mx_vif_unpack(MX_UNPACK_V4_32, B, 2, 0));
  c = mx_e_w4(c, n, ctx, (u32)kick, V0 + iters(k, n) * k->u * k->stride);
  c = mx_e_giftag(c, n, 1, (s32)prim(k), MX_GIF_FLG_PACKED, nreg(k), regs(k));
  c = mx_e_vif4(c, mx_vif_stcycl(k->stride, 1), 0, 0, 0);
  if (fmt == C16)
    c = e_stream(c, k, m->pos16[v], 6, nn, 1, MX_UNPACK_V3_16, V0 + (u32)k->pos, 0);
  else
    c = e_stream(c, k, m->pos32[v], 12, nn, 0, MX_UNPACK_V3_32, V0 + (u32)k->pos, 0);
  if (k->st >= 0 && k - vx == S_TEX16)
    c = e_stream(c, k, m->st16[v], 4, nn, 0, MX_UNPACK_V2_16, V0 + (u32)k->st, MX_UNPACK_MASK);
  else if (k->st >= 0)
    c = e_stream(c, k, m->st[v], 8, nn, 0, MX_UNPACK_V2_32, V0 + (u32)k->st, MX_UNPACK_MASK);
  if (k->col >= 0)
    c = e_stream(c, k, &m->col[v], 4, nn, 0, MX_UNPACK_V4_8, V0 + (u32)k->col, MX_UNPACK_USN);
  if (k->nrm >= 0)
    c = e_stream(c, k, m->nrm[v], 12, nn, 0, MX_UNPACK_V3_32, V0 + (u32)k->nrm, 0);
  return mx_e_cnt(c, 0, mx_vif_itop(B), mx_vif_mscal(k->at));
}
#define BATCH_QW 13

/* ---- uniforms ------------------------------------------------------------------------- */

/* A projection: offset-free pixel coordinates times w, a depth times w, w;
   columns for x, y, z and 1 of the object. */
typedef float mat4[4][4];

union fu {
  float f;
  u32 u;
};
static u32 fbits(float f) {
  union fu x = {f};
  return x.u;
}

struct light {
  float dir[3][3]; /* towards the lights (S4/S5: the first) */
  float col[3][3]; /* 0..255 */
  float amb[3];
};

/* The uniforms of kernel s for projection pc (the VSM layout,
   tools/vusched/vusched.py) into u; returns the quadwords. */
static u32 vx_uniforms(int s, int fmt, float p16, const mat4 pc, const struct light *l,
                       u32 (*u)[4]) {
  mat4 m;
  for (int c = 0; c < 4; c++)
    for (int r = 0; r < 4; r++)
      m[c][r] = pc[c][r];
  if (s == S_SCEI) {
    /* T = P / w in pixels * 16, offset-free (CLIPW tests it) */
    for (int c = 0; c < 4; c++)
      for (int r = 0; r < 3; r++)
        m[c][r] *= 16.0f;
  } else {
    /* P / w are GS coordinates: + 2048 w in x and y */
    for (int c = 0; c < 4; c++)
      m[c][0] += 2048.0f * m[c][3], m[c][1] += 2048.0f * m[c][3];
  }
  if (fmt == C16) {
    /* the VU reads 1.5 * 2^23 + p16 * p */
    for (int c = 0; c < 3; c++)
      for (int r = 0; r < 4; r++)
        m[c][r] /= p16;
    for (int r = 0; r < 4; r++)
      m[3][r] -= MAGIC * (m[0][r] + m[1][r] + m[2][r]);
  }
  for (int c = 0; c < 4; c++)
    for (int r = 0; r < 4; r++)
      u[c][r] = fbits(m[c][r]);
  if (s == S_LIT || s == S_LITTEX) {
    for (int k = 0; k < 3; k++) {
      for (int r = 0; r < 3; r++)
        u[4 + k][r] = fbits(l->col[0][r] * l->dir[0][k]);
      u[4 + k][3] = 0;
    }
    for (int r = 0; r < 3; r++)
      u[7][r] = fbits(l->amb[r] + MAGIC);
    u[7][3] = 0x80; /* alpha, an integer */
    return 8;
  }
  if (s == S_SCEI) {
    for (int k = 0; k < 3; k++) {
      for (int j = 0; j < 3; j++)
        u[4 + k][j] = fbits(l->dir[j][k]); /* lane j: light j */
      u[4 + k][3] = 0;
      for (int r = 0; r < 3; r++)
        u[7 + k][r] = fbits(l->col[k][r]);
      u[7 + k][3] = 0;
    }
    for (int r = 0; r < 3; r++) {
      u[10][r] = fbits(l->amb[r] + MAGIC);
      u[11][r] = fbits(MAGIC + 255.0f);
    }
    u[10][3] = 0x80, u[11][3] = 0;
    u[12][0] = u[12][1] = fbits(MAGIC + 2048.0f * 16.0f);
    u[12][2] = fbits(MAGIC), u[12][3] = 0;
    u[13][0] = u[13][1] = u[13][2] = 0, u[13][3] = fbits(GUARD * 16.0f);
    return 14;
  }
  return 4;
}

/* The same for the HLSL twin of s (natural values: no magic numbers). */
static u32 vh_uniforms(int s, const mat4 pc, const struct light *l, float (*u)[4]) {
  for (int c = 0; c < 4; c++)
    for (int r = 0; r < 4; r++)
      u[c][r] = pc[c][r] + (s != S_SCEI && r < 2 ? 2048.0f * pc[c][3] : 0.0f);
  if (s == S_LIT || s == S_LITTEX) {
    for (int k = 0; k < 3; k++) {
      for (int r = 0; r < 3; r++)
        u[4 + k][r] = l->col[0][r] * l->dir[0][k];
      u[4 + k][3] = 0;
    }
    for (int r = 0; r < 3; r++)
      u[7][r] = l->amb[r];
    u[7][3] = 128.0f;
    return 8;
  }
  if (s == S_SCEI) {
    for (int k = 0; k < 3; k++) {
      for (int j = 0; j < 3; j++)
        u[4 + k][j] = l->dir[j][k];
      u[4 + k][3] = 0;
      for (int r = 0; r < 3; r++)
        u[7 + k][r] = l->col[k][r];
      u[7 + k][3] = 0;
    }
    for (int r = 0; r < 3; r++)
      u[10][r] = l->amb[r];
    u[10][3] = 128.0f;
    u[11][0] = u[11][1] = 2048.0f, u[11][2] = 0, u[11][3] = GUARD;
    return 12;
  }
  return 4;
}

/* ---- math without libm ------------------------------------------------------------------ */

static float sinf_(float a) {
  while (a > 3.14159265f)
    a -= 6.28318531f;
  while (a < -3.14159265f)
    a += 6.28318531f;
  float a2 = a * a;
  return a * (1 - a2 / 6 * (1 - a2 / 20 * (1 - a2 / 42 * (1 - a2 / 72))));
}
static float cosf_(float a) { return sinf_(a + 1.57079633f); }
static float rsqrt_(float l) {
  float r = 1;
  if (l > 4)
    r = 0.25f;
  for (int k = 0; k < 12; k++)
    r = r * (1.5f - 0.5f * l * r * r);
  return r;
}
static float fabsf_(float a) { return a < 0 ? -a : a; }

/* ---- the scene: terrain patches (levels.c) ------------------------------------------------- */

static float s_pos32[NV + 16][3] MX_ALIGNED(16);
static s16 s_pos16[NV + 16][3] MX_ALIGNED(16);
static u32 s_col[NV + 16] MX_ALIGNED(16);
static float s_nrm[NV + 16][3] MX_ALIGNED(16);
static float s_st[NV + 16][2] MX_ALIGNED(16);
static s16 s_st16[NV + 16][2] MX_ALIGNED(16);
static const struct mesh scene = {s_pos32, s_pos16, s_col, s_nrm, s_st, s_st16, 512.0f};

static void scene_vertex(u32 k, int x, int z) {
  float fx = (float)x, fz = (float)z;
  float h = 3 * sinf_(0.4f * fx) * cosf_(0.3f * fz);
  float dx = 1.2f * cosf_(0.4f * fx) * cosf_(0.3f * fz);
  float dz = -0.9f * sinf_(0.4f * fx) * sinf_(0.3f * fz);
  float s = rsqrt_(dx * dx + 1 + dz * dz);
  s_pos16[k][0] = (s16)(s32)(fx * 512), s_pos16[k][1] = (s16)(s32)(h * 512);
  s_pos16[k][2] = (s16)(s32)(fz * 512);
  for (int i = 0; i < 3; i++)
    s_pos32[k][i] = (float)s_pos16[k][i] / 512.0f;
  s_nrm[k][0] = -dx * s, s_nrm[k][1] = s, s_nrm[k][2] = -dz * s;
  u32 g = (u32)(s32)(128 + 40 * h);
  s_col[k] = 0x80000000u | (g / 2) << 16 | g << 8 | (255 - g);
  s_st16[k][0] = (s16)(x * 512), s_st16[k][1] = (s16)(z * 512);
  s_st[k][0] = (float)s_st16[k][0] / 4096, s_st[k][1] = (float)s_st16[k][1] / 4096;
}

static void make_scene(void) {
  u32 k = 0;
  for (int r = 0; r < G - 1; r++)
    for (int x = 0; x < G; x++)
      for (int d = 0; d < 2; d++) {
        /* rows joined by the last vertex again and the next one twice */
        if (r && !x && !d)
          scene_vertex(k, G - 1, r), k++;
        scene_vertex(k++, x, r + d);
        if (r && !x && !d)
          scene_vertex(k, x, r + d), k++;
      }
  for (; k < NV + 16; k++)
    scene_vertex(k, G - 1, G - 1);
  mx_dcache_writeback(s_pos32, sizeof s_pos32);
  mx_dcache_writeback(s_pos16, sizeof s_pos16);
  mx_dcache_writeback(s_col, sizeof s_col);
  mx_dcache_writeback(s_nrm, sizeof s_nrm);
  mx_dcache_writeback(s_st, sizeof s_st);
  mx_dcache_writeback(s_st16, sizeof s_st16);
}

static const struct light scene_light = {
    {{0.30f, 0.90f, 0.30f}, {-0.70f, 0.50f, 0.20f}, {0.10f, 0.40f, -0.90f}},
    {{180, 170, 160}, {150, 110, 60}, {40, 60, 140}},
    {60, 60, 70}};

/* Patch i of the scene: tiles of 80 x 56 pixels with a little perspective
   (w grows with z), in rows over the 640 x 224 screen. */
static void patch_proj(int i, mat4 pc) {
  const u32 tw = 80, th = 56, cols = 640 / tw;
  u32 t = (u32)i % (cols * (224 / th));
  float ox = (float)(s32)(t % cols * tw) - 320, oy = (float)(s32)(t / cols * th) - 112;
  float sx = (float)tw / (G - 1), sz = (float)th / (G - 1), kw = 0.004f;
  for (int c = 0; c < 4; c++)
    for (int r = 0; r < 4; r++)
      pc[c][r] = 0;
  /* x = ox + sx x / w, y = oy + (sz z - 1.5 h) / w, depth 100 + 10 h / w, w = 1 + kw z */
  pc[0][0] = sx;
  pc[1][1] = -1.5f, pc[1][2] = 10;
  pc[2][0] = ox * kw, pc[2][1] = oy * kw + sz, pc[2][2] = 100 * kw, pc[2][3] = kw;
  pc[3][0] = ox, pc[3][1] = oy, pc[3][2] = 100, pc[3][3] = 1;
}

/* ---- part 1: every batch size against the EE ------------------------------------------------- */

static float t_pos32[NT + 16][3] MX_ALIGNED(16);
static s16 t_pos16[NT + 16][3] MX_ALIGNED(16);
static u32 t_col[NT + 16] MX_ALIGNED(16);
static float t_nrm[NT + 16][3] MX_ALIGNED(16);
static float t_st[NT + 16][2] MX_ALIGNED(16);
static s16 t_st16[NT + 16][2] MX_ALIGNED(16);
static const struct mesh test = {t_pos32, t_pos16, t_col, t_nrm, t_st, t_st16, 8192.0f};
static u64 chain[2 * 64] MX_ALIGNED(64);
static u32 out[BUF_QW][4] MX_ALIGNED(64);
static u32 uni[CTX_QW][4] MX_ALIGNED(16);

static u32 seed = 1;
static u32 rnd32(void) {
  seed = seed * 1103515245u + 12345u;
  return seed;
}
static float rnd(void) { return (float)(s32)(rnd32() >> 9 & 0xFFFF) / 32768.0f - 1.0f; }

static void make_test(void) {
  for (u32 i = 0; i < NT + 16; i++) {
    for (int k = 0; k < 3; k++) {
      t_pos16[i][k] = (s16)(s32)(rnd() * 1.5f * 8192);
      t_pos32[i][k] = (float)t_pos16[i][k] / 8192.0f;
    }
    float n[3] = {rnd(), rnd(), rnd()}, l = rsqrt_(n[0] * n[0] + n[1] * n[1] + n[2] * n[2] + 1e-6f);
    for (int k = 0; k < 3; k++)
      t_nrm[i][k] = n[k] * l;
    t_col[i] = rnd32();
    t_st16[i][0] = (s16)(s32)(rnd() * 4 * 4096), t_st16[i][1] = (s16)(s32)(rnd() * 4 * 4096);
    t_st[i][0] = (float)t_st16[i][0] / 4096, t_st[i][1] = (float)t_st16[i][1] / 4096;
  }
  mx_dcache_writeback(t_pos32, sizeof t_pos32);
  mx_dcache_writeback(t_pos16, sizeof t_pos16);
  mx_dcache_writeback(t_col, sizeof t_col);
  mx_dcache_writeback(t_nrm, sizeof t_nrm);
  mx_dcache_writeback(t_st, sizeof t_st);
  mx_dcache_writeback(t_st16, sizeof t_st16);
}

/* x in [-1.5, 1.5]: up to 3000 pixels from the centre, beyond the guard band */
static void test_proj(mat4 pc) {
  for (int c = 0; c < 4; c++)
    for (int r = 0; r < 4; r++)
      pc[c][r] = 0;
  pc[0][0] = 2500, pc[0][1] = -50;
  pc[1][1] = 2500, pc[1][2] = 200;
  pc[2][0] = 100, pc[2][2] = 300, pc[2][3] = 0.5f;
  pc[3][2] = 400, pc[3][3] = 2;
}

static int bad, bad0;
static void fail(const struct vx *k, int fmt, u32 n, u32 i, const char *what, s32 got, s32 want) {
  if (bad++ - bad0 < 6)
    mx_printf("%s %s n %u vertex %u: %s %d, expected %d\n", k->name, fmt_name[fmt], n, i, what, got,
              want);
}
static void near_i(const struct vx *k, int fmt, u32 n, u32 i, const char *what, s32 got, s32 want,
                   s32 tol) {
  s32 d = got - want;
  if (d > tol || -d > tol)
    fail(k, fmt, n, i, what, got, want);
}
/* the low 16 bits (XYZ2 X and Y) */
static void near16(const struct vx *k, int fmt, u32 n, u32 i, const char *what, u32 got, s32 want,
                   s32 tol) {
  s32 d = (s16)(u16)(got - (u32)want);
  if (d > tol || -d > tol)
    fail(k, fmt, n, i, what, (s32)(got & 0xFFFF), want & 0xFFFF);
}
static void near_f(const struct vx *k, int fmt, u32 n, u32 i, const char *what, u32 got, float want) {
  union fu g = {.u = got};
  float tol = 1e-4f * fabsf_(want) + 1e-6f;
  if (fabsf_(g.f - want) > tol)
    fail(k, fmt, n, i, what, (s32)(g.f * 1e6f), (s32)(want * 1e6f));
}

/* Run one batch of n test vertices with kernel k in buffer b (no XGKICK);
   returns 0 when it ended. */
static int run_batch(const struct vx *k, int fmt, const u32 (*u)[4], u32 nq, u32 n, u32 b) {
  static u32 mark = 0x100;
  mark = (mark + 1) & 0xFFFF;
  u64 *c = chain;
  c = e_vif_setup(c, k - vx == S_TEX16);
  c = e_context(c, 16 * (n & 1), u, nq);
  c = e_batch(c, k, fmt, &test, b, 16 * (n & 1), 0, n, 0);
  c = mx_e_end(c, 0, mx_vif_flushe(), mx_vif_mark(mark));
  mx_dcache_writeback(chain, sizeof chain);
  mx_dma_chain(MX_CH_VIF1, chain, 1);
  u32 t0 = mx_count();
  while ((MX_R32(MX_VIF_VIF1_MARK) & 0xFFFF) != mark)
    if (mx_count() - t0 > 100000000u)
      return -1;
  mx_dma_wait(MX_CH_VIF1);
  return 0;
}

/* Run the batch of n that is in buffer b again (its data is the output now,
   the timing is the same; the header again, the first iterations stored
   over it): MSCAL to the MARK after FLUSHE, EE cycles. */
static u32 rerun(const struct vx *k, u32 n, u32 b) {
  static u32 mark = 0x8000;
  mark = (mark + 1) & 0xFFFF;
  u32 B = hdr_at(k, b);
  u64 *c = chain;
  c = mx_e_cnt(c, 2, mx_vif_stcycl(1, 1), mx_vif_unpack(MX_UNPACK_V4_32, B, 2, 0));
  c = mx_e_w4(c, n, 0, 0, B + 2 + iters(k, n) * k->u * k->stride);
  c = mx_e_giftag(c, n, 1, (s32)prim(k), MX_GIF_FLG_PACKED, nreg(k), regs(k));
  c = mx_e_end(c, 1, mx_vif_itop(B), mx_vif_mscal(k->at));
  c = mx_e_vif4(c, mx_vif_flushe(), mx_vif_mark(mark), 0, 0);
  mx_dcache_writeback(chain, sizeof chain);
  u32 t0 = mx_count();
  mx_dma_chain(MX_CH_VIF1, chain, 1);
  while ((MX_R32(MX_VIF_VIF1_MARK) & 0xFFFF) != mark)
    if (mx_count() - t0 > 100000000u)
      return 0;
  u32 t = mx_count() - t0;
  mx_dma_wait(MX_CH_VIF1);
  return t;
}

/* 16 batches of each kernel in one plain VIF1 chain (no frame pipeline, no
   XGKICK), the buffers by turns: DMA start to the MARK after FLUSHE. */
static u64 bchain[2 * 16 * 24] MX_ALIGNED(64);
static void batches_chain(void) {
  mat4 pc;
  test_proj(pc);
  for (int s = 0; s < NS; s++) {
    const struct vx *k = &vx[s];
    for (int fmt = 0; fmt < NF; fmt++) {
      u32 nq = vx_uniforms(s, fmt, test.p16, pc, &scene_light, uni);
      mx_dcache_writeback(uni, sizeof uni);
      u32 n = max_n(k), v[5];
      for (int r = 0; r < 5; r++) {
        static u32 mark = 0x4000;
        mark = (mark + 1) & 0xFFFF;
        u64 *c = bchain;
        c = e_vif_setup(c, s == S_TEX16);
        c = e_context(c, 0, (const u32(*)[4])uni, nq);
        for (u32 b = 0; b < 16; b++)
          c = e_batch(c, k, fmt, &test, b % 3, 0, 0, n, 0);
        c = mx_e_end(c, 0, mx_vif_flushe(), mx_vif_mark(mark));
        mx_dcache_writeback(bchain, sizeof bchain);
        u32 t0 = mx_count();
        mx_dma_chain(MX_CH_VIF1, bchain, 1);
        while ((MX_R32(MX_VIF_VIF1_MARK) & 0xFFFF) != mark)
          if (mx_count() - t0 > 100000000u)
            break;
        v[r] = (mx_count() - t0) * 100 / (16 * n);
        mx_dma_wait(MX_CH_VIF1);
      }
      for (int a = 1; a < 5; a++)
        for (int b = a; b > 0 && v[b] < v[b - 1]; b--) {
          u32 x = v[b];
          v[b] = v[b - 1];
          v[b - 1] = x;
        }
      mx_printf("vumax chain %-10s %s: 16 batches of %u: %u.%02u cycles/vertex\n", k->name,
                fmt_name[fmt], n, v[2] / 100, v[2] % 100);
    }
  }
}

/* The kernels alone (data in VU1 memory, no VIF data, no XGKICK): cycles
   per vertex from the difference of a small and a large batch. */
static void kernels_alone(void) {
  mat4 pc;
  test_proj(pc);
  for (int s = 0; s < NS; s++) {
    const struct vx *k = &vx[s];
    u32 nq = vx_uniforms(s, F32, test.p16, pc, &scene_light, uni);
    mx_dcache_writeback(uni, sizeof uni);
    u32 n1 = 8 * k->u, n2 = max_n(k) / k->u * k->u, t[2];
    for (int j = 0; j < 2; j++) {
      u32 n = j ? n2 : n1, v[5];
      run_batch(k, F32, (const u32(*)[4])uni, nq, n, 0);
      for (int r = 0; r < 5; r++)
        v[r] = rerun(k, n, 0);
      for (int a = 1; a < 5; a++)
        for (int b = a; b > 0 && v[b] < v[b - 1]; b--) {
          u32 x = v[b];
          v[b] = v[b - 1];
          v[b - 1] = x;
        }
      t[j] = v[2];
    }
    u32 c100 = (t[1] - t[0]) * 100 / (n2 - n1);
    mx_printf("vumax kernel %-10s alone: %u vertices %u cycles, %u vertices %u cycles: %u.%02u "
              "cycles/vertex (II/u %u.%u)\n",
              k->name, n1, t[0], n2, t[1], c100 / 100, c100 % 100, k->ii / k->u,
              k->ii * 10 / k->u % 10);
  }
}

/* The outputs of n test vertices of kernel k (or of its HLSL twin, hlsl: the
   same registers in the same order) from v, against the EE. */
static void check_vertices(const struct vx *k, int fmt, int hlsl, u32 n, const u32 (*v)[4],
                           const mat4 pc, const struct light *l) {
  int cut[NT + 2] = {0};
  for (u32 i = 0; i < n; i++) {
    const u32 *o = v[i * k->stride];
    float x = test.pos32[i][0], y = test.pos32[i][1], z = test.pos32[i][2], p[4];
    for (int r = 0; r < 4; r++)
      p[r] = pc[0][r] * x + pc[1][r] * y + pc[2][r] * z + pc[3][r];
    float q = 1.0f / p[3];
    /* c16: the 1.5 * 2^23 in the MVP costs ~1.5 LSB of the 16-bit position,
       which is 0.3 pixels in this projection (2500 pixels per unit) */
    s32 tol = fmt == C16 ? 16 : 2;
    const u32 *xyz = o + 4 * k->pos;
    if (k - vx == S_SCEI) {
      float t16[3] = {16 * p[0] * q, 16 * p[1] * q, 16 * p[2] * q};
      int near = 0;
      cut[i + 2] = 0;
      for (int j = 0; j < 3; j++) {
        float e = fabsf_(t16[j]) - GUARD * 16;
        cut[i + 2] |= e > 0;
        near |= fabsf_(e) < 64;
      }
      near16(k, fmt, n, i, "X", xyz[0], (s32)t16[0] + 32768, tol);
      near16(k, fmt, n, i, "Y", xyz[1], (s32)t16[1] + 32768, tol);
      /* the kernel's Z has the magic number, the twin's is FTOI4 */
      near_i(k, fmt, n, i, "Z", (s32)(xyz[2] - (hlsl ? 0 : MAGIC_BITS)), (s32)t16[2], tol);
      int adc = (xyz[3] >> 15) & 1, want = cut[i + 2] | cut[i + 1] | cut[i];
      /* the first two vertices see the flags of the iterations before vertex
         0 (they end no triangle); vertices at the guard band may go either way */
      if (i >= 2 && !near && adc != want)
        fail(k, fmt, n, i, "ADC", adc, want);
    } else {
      near16(k, fmt, n, i, "X", xyz[0], (s32)(16 * (p[0] * q + 2048)), tol);
      near16(k, fmt, n, i, "Y", xyz[1], (s32)(16 * (p[1] * q + 2048)), tol);
      float zw = 16 * p[2] * q;
      near_i(k, fmt, n, i, "Z", (s32)xyz[2], (s32)zw, tol + (s32)(fabsf_(zw) * 1e-5f));
      if (xyz[3] & 0x8000)
        fail(k, fmt, n, i, "ADC", 1, 0);
    }
    if (k->st >= 0) {
      const u32 *st = o + 4 * k->st;
      near_f(k, fmt, n, i, "S", st[0], test.st[i][0] * q);
      near_f(k, fmt, n, i, "T", st[1], test.st[i][1] * q);
      near_f(k, fmt, n, i, "Q", st[2], q);
    }
    if (k->col >= 0) {
      const u32 *rgba = o + 4 * k->col;
      for (int j = 0; j < 4; j++)
        if (rgba[j] != (test.col[i] >> (8 * j) & 0xFF))
          fail(k, fmt, n, i, "colour", (s32)rgba[j], (s32)(test.col[i] >> (8 * j) & 0xFF));
    }
    if (k->nrm >= 0) {
      const u32 *rgba = o + 4 * k->nrm;
      const float *nv = test.nrm[i];
      for (int j = 0; j < 3; j++) {
        float v;
        if (k - vx == S_SCEI) {
          v = l->amb[j];
          for (int m = 0; m < 3; m++) {
            float cs = l->dir[m][0] * nv[0] + l->dir[m][1] * nv[1] + l->dir[m][2] * nv[2];
            v += l->col[m][j] * (cs > 0 ? cs : 0);
          }
          if (v > 255)
            v = 255;
        } else {
          float cs = l->dir[0][0] * nv[0] + l->dir[0][1] * nv[1] + l->dir[0][2] * nv[2];
          v = l->amb[j] + l->col[0][j] * (cs > 0 ? cs : 0);
        }
        near_i(k, fmt, n, i, "lit colour", (s32)(rgba[j] & 0xFF), (s32)v, 1);
      }
      if ((rgba[3] & 0xFF) != 0x80)
        fail(k, fmt, n, i, "alpha", (s32)(rgba[3] & 0xFF), 0x80);
    }
  }
}

static void check_batch(const struct vx *k, int fmt, u32 n, u32 b, const mat4 pc,
                        const struct light *l) {
  u32 B = hdr_at(k, b);
  mx_vu1_read(out, B, 2 + n * k->stride);
  /* the GIF tag is written again after the loop */
  u64 tag = mx_giftag(n, 1, (s32)prim(k), MX_GIF_FLG_PACKED, nreg(k));
  if (out[1][0] != (u32)tag || out[1][1] != (u32)(tag >> 32) || out[1][2] != (u32)regs(k))
    fail(k, fmt, n, 0, "GIF tag", (s32)out[1][0], (s32)(u32)tag);
  check_vertices(k, fmt, 0, n, (const u32(*)[4])out[2], pc, l);
}

/* One batch of n test vertices with HLSL program h (as tests/ps2/shader.c):
   returns the buffer (VIF1 TOP), or -1 when the program did not end. */
static float uni_t[CTX_QW][4] MX_ALIGNED(16);
static s32 run_vh(const struct mx_shader *h, int s, u32 n) {
  static u32 mark = 0x300;
  mark = (mark + 1) & 0xFFFF;
  const void *streams[3];
  u32 k = 0;
  streams[k++] = test.pos32;
  if (s == S_GOUR || s == S_TEX || s == S_TEX16)
    streams[k++] = test.col;
  if (s >= S_LIT && s <= S_SCEI)
    streams[k++] = test.nrm;
  if (s == S_TEX || s == S_LITTEX || s == S_SCEI)
    streams[k++] = test.st;
  if (s == S_TEX16)
    streams[k++] = test.st16;
  u64 *c = chain;
  c = mx_e_cnt(c, 1, mx_vif_base(0), mx_vif_offset(h->buf_qw));
  c = mx_e_vif4(c, mx_vif_stcycl(1, 1), mx_vif_stmod(0), 0, 0);
  c = mx_shader_e_context(c, h, mx_shader_ctx(h, 0), prim(&vx[s]), uni_t);
  c = mx_shader_e_batch(c, h, streams, n, 1);
  c = mx_e_end(c, 0, mx_vif_flush(), mx_vif_mark(mark));
  mx_dcache_writeback(chain, sizeof chain);
  mx_dma_chain(MX_CH_VIF1, chain, 1);
  u32 t0 = mx_count();
  while ((MX_R32(MX_VIF_VIF1_MARK) & 0xFFFF) != mark)
    if (mx_count() - t0 > 100000000u)
      return -1;
  mx_dma_wait(MX_CH_VIF1);
  return (s32)(MX_R32(MX_VIF_VIF1_TOP) & 0x3FF);
}

/* Every batch size of the HLSL twin of s. */
static int part1_vh(int s, const struct mx_shader *h, const mat4 pc) {
  const struct vx *k = &vx[s];
  if (h->regs != regs(k) || h->nreg != nreg(k)) {
    mx_printf("%s: the HLSL twin writes other registers (0x%x)\n", k->name, (u32)h->regs);
    return 1;
  }
  vh_uniforms(s, pc, &scene_light, uni_t);
  mx_dcache_writeback(uni_t, sizeof uni_t);
  mx_vu_load(1, h->code, h->code_bytes, h->entry);
  u32 b0 = (u32)bad;
  bad0 = bad;
  u32 nmax = h->max_n < NT ? h->max_n : NT;
  for (u32 n = 1; n <= nmax; n++) {
    s32 top = run_vh(h, s, n);
    if (top < 0) {
      mx_printf("vh %s n %u: the batch did not end\n", k->name, n);
      return 1;
    }
    mx_vu1_read(out, (u32)top + h->out_at, 1 + n * h->out_qw);
    if ((out[0][0] & 0x7FFF) != n)
      fail(k, F32, n, 0, "NLOOP", (s32)(out[0][0] & 0x7FFF), (s32)n);
    check_vertices(k, F32, 1, n, (const u32(*)[4])out[1], pc, &scene_light);
  }
  mx_printf("vh %s: batches of 1..%u %s\n", k->name, nmax,
            (u32)bad == b0 ? "agree with the EE" : "FAIL");
  return 0;
}

static int part1(const struct mx_shader *sh) {
  mat4 pc;
  test_proj(pc);
  for (int s = 0; s < NS; s++) {
    const struct vx *k = &vx[s];
    for (int fmt = 0; fmt < NF; fmt++) {
      u32 nq = vx_uniforms(s, fmt, test.p16, pc, &scene_light, uni);
      mx_dcache_writeback(uni, sizeof uni);
      u32 nmax = max_n(k), b0 = (u32)bad;
      bad0 = bad;
      for (u32 n = 1; n <= nmax; n++) {
        u32 b = n % 3;
        if (run_batch(k, fmt, (const u32(*)[4])uni, nq, n, b) < 0) {
          mx_printf("%s %s n %u: the batch did not end\n", k->name, fmt_name[fmt], n);
          return 1;
        }
        check_batch(k, fmt, n, b, pc, &scene_light);
      }
      mx_printf("%s %s: batches of 1..%u %s\n", k->name, fmt_name[fmt], nmax,
                (u32)bad == b0 ? "agree with the EE" : "FAIL");
    }
    if (part1_vh(s, &sh[s], pc))
      return 1;
  }
  return bad != 0;
}

/* ---- part 2: throughput ------------------------------------------------------------------------- */

static u8 ring[512 * 1024] MX_ALIGNED(512 * 1024);
static u32 ring_kb = 64;
static u32 tex[64 * 64] MX_ALIGNED(64);
static u32 ctx_turn, buf_turn, cap_n = 1024;

/* the GS state of a frame: texture, the colour of S1 */
static void frame_env(void) {
  u64 *c = mx_frame_direct(5);
  c = mx_e_ad_tag(c, 4, 1);
  c = mx_e_ad(c, MX_GS_REG_TEX0_1, mx_gs_tex0(mx_gs.tbp, 1, MX_GS_PSM_CT32, 6, 6, 0, 0));
  c = mx_e_ad(c, MX_GS_REG_TEX1_1, mx_gs_tex1(0, 0, 1, 1, 0, 0, 0));
  c = mx_e_ad(c, MX_GS_REG_CLAMP_1, mx_gs_clamp(0, 0, 0, 0, 0, 0));
  c = mx_e_ad(c, MX_GS_REG_RGBAQ, mx_gs_rgbaq(0x40, 0xA0, 0x60, 0x80, 1.0f));
  mx_frame_commit(c);
}

/* One patch with kernel k: its context, then batches of a strip that
   overlap by two vertices and start on multiples of 8 (V3-16 is 6 bytes). */
static void vx_draw(const struct vx *k, int fmt, int kick, const u32 (*u)[4], u32 nq) {
  u32 ctx = 16 * (ctx_turn ^= 1), size = k->max_n < cap_n ? k->max_n : cap_n;
  while ((size - 2) % 8)
    size--;
  u64 *c = mx_frame_reserve(nq + 2);
  mx_frame_commit(e_context(c, ctx, u, nq));
  for (u32 v = 0;; v += size - 2) {
    u32 n = NV - v < size ? NV - v : size;
    buf_turn = buf_turn == 2 ? 0 : buf_turn + 1;
    c = mx_frame_reserve(BATCH_QW);
    mx_frame_commit(e_batch(c, k, fmt, &scene, buf_turn, ctx, v, n, kick));
    if (v + n >= NV)
      break;
  }
  /* STMOD back to normal for whatever comes next */
  mx_frame_commit(mx_e_cnt(mx_frame_reserve(1), 0, mx_vif_stmod(0), 0));
}

struct result {
  u32 cycles;               /* per frame */
  u32 wait_ring, wait_buf;  /* EE, per frame */
  u32 build;                /* EE in frame(), per frame */
  u32 busy[MX_TS_N], ns;
};

static u32 uni_f[PATCHES][CTX_QW][4] MX_ALIGNED(16);
static float uni_h[PATCHES][CTX_QW][4] MX_ALIGNED(16);

/* Frames of scenario s: vx kernel (hlsl = 0) or its HLSL twin. */
static void frame(int s, int fmt, int kick, const struct mx_shader *h, u32 nq) {
  mx_frame_begin(0x10, 0x18, 0x30);
  frame_env();
  if (!h) {
    u64 *c = mx_frame_reserve(VIF_SETUP_QW);
    mx_frame_commit(e_vif_setup(c, s == S_TEX16));
    for (int i = 0; i < PATCHES; i++)
      vx_draw(&vx[s], fmt, kick, (const u32(*)[4])uni_f[i], nq);
  } else {
    const void *streams[3];
    u32 k = 0;
    streams[k++] = scene.pos32;
    if (s == S_GOUR || s == S_TEX || s == S_TEX16)
      streams[k++] = scene.col;
    if (s >= S_LIT && s <= S_SCEI)
      streams[k++] = scene.nrm;
    if (s == S_TEX || s == S_LITTEX || s == S_SCEI)
      streams[k++] = scene.st;
    if (s == S_TEX16)
      streams[k++] = scene.st16;
    for (int i = 0; i < PATCHES; i++)
      mx_frame_shader_draw(h, prim(&vx[s]), uni_h[i], streams, NV);
  }
  mx_frame_end();
}

static void measure(int s, int fmt, int kick, const struct mx_shader *h, struct result *r) {
  mat4 pc;
  u32 nq = 0;
  for (int i = 0; i < PATCHES; i++) {
    patch_proj(i, pc);
    if (h)
      nq = vh_uniforms(s, pc, &scene_light, uni_h[i]);
    else
      nq = vx_uniforms(s, fmt, scene.p16, pc, &scene_light, uni_f[i]);
  }
  mx_dcache_writeback(uni_f, sizeof uni_f);
  mx_dcache_writeback(uni_h, sizeof uni_h);
  if (h)
    mx_frame_shader_setup(h);
  else
    mx_frame_mpg(vx[s].code, (u32)(vx[s].end - vx[s].code), vx[s].at);
  u32 base = mx_gs.frames;
  frame(s, fmt, kick, h, nq); /* warm up */
  u32 d0 = mx_count();
  while (mx_gs.frames_done < base + 1 && mx_count() - d0 < 300000000u)
    ;
  base = mx_gs.frames;
  mx_trace_sample(2000);
  u32 t0 = mx_count(), wr = mx_frame_stats.wait_ring, wb = mx_frame_stats.wait_buffer;
  u32 build = 0;
  for (int f = 0; f < FRAMES; f++) {
    u32 b0 = mx_count();
    frame(s, fmt, kick, h, nq);
    build += mx_count() - b0;
  }
  r->build = build / FRAMES;
  d0 = mx_count();
  while (mx_gs.frames_done < base + FRAMES && mx_count() - d0 < 300000000u)
    ;
  r->cycles = (mx_count() - t0) / FRAMES;
  r->wait_ring = (mx_frame_stats.wait_ring - wr) / FRAMES;
  r->wait_buf = (mx_frame_stats.wait_buffer - wb) / FRAMES;
  r->ns = mx_trace_busy(r->busy);
  mx_trace_sample(0);
}

static const char *const sc_name[NS] = {"S1 flat", "S2 gouraud", "S3 tex", "S4 lit", "S5 littex",
                                        "S6 scei", "S3b tex16"};

static void print_row(const char *name, const char *how, const struct result *r) {
  u32 verts = PATCHES * NV, c100 = (u32)((u64)r->cycles * 100 / verts);
  u32 mvs = (u32)((u64)MX_EE_HZ * 10 / ((u64)r->cycles * 1000000 / verts));
  mx_printf("vumax %-10s %-16s %3u.%02u cycles/vertex %3u.%u Mvertices/s", name, how, c100 / 100,
            c100 % 100, mvs / 10, mvs % 10);
  static const char *const u[MX_TS_N] = {"VIF0", "VIF1", "GIF DMA", "fromSPR", "toSPR",
                                         "VU0",  "VU1",  "GIF",     "GS FIFO", "VU1 GIF wait"};
  for (int i = 1; i < MX_TS_N; i++)
    if (i != 5 && i != 2)
      mx_printf(" %s %u%%", u[i], r->ns ? r->busy[i] * 100 / r->ns : 0);
  mx_printf(" EE in frame() %uk, ring wait %uk, buffer wait %uk", r->build / 1000,
            r->wait_ring / 1000, r->wait_buf / 1000);
  mx_printf("\n");
}

/* Options: s<k> only scenario k (1..7, 7: S3b, repeatable), n<N> at most N vertices
   per batch, t only the throughput (no part 1), k only the kernels alone, c only
   batches in a plain chain, r<KB> ring size (a power of two, 64 .. 512). */
int main(int argc, char **argv) {
  u32 only = 0;
  int quick = 0;
  for (int a = 1; a < argc; a++) {
    const char *p = argv[a];
    if (p[0] == 's' && p[1] >= '1' && p[1] <= '7')
      only |= 1u << (p[1] - '1');
    else if (p[0] == 'n') {
      cap_n = 0;
      for (p++; *p >= '0' && *p <= '9'; p++)
        cap_n = cap_n * 10 + (u32)(*p - '0');
    } else if (p[0] == 't')
      quick = 1;
    else if (p[0] == 'k')
      quick = 2;
    else if (p[0] == 'c')
      quick = 3;
    else if (p[0] == 'r') {
      ring_kb = 0;
      for (p++; *p >= '0' && *p <= '9'; p++)
        ring_kb = ring_kb * 10 + (u32)(*p - '0');
    }
  }
  if (!only)
    only = (1u << NS) - 1;
  mx_gs_init(640, 224);
  mx_vu_reset(2);
  vx_slots();
  vx_batches();
  make_scene();
  make_test();
  for (int s = 0; s < NS; s++)
    mx_vu_load(1, vx[s].code, (u32)(vx[s].end - vx[s].code), vx[s].at);
  mx_printf("vumax: ring %p (%u KiB), scene pos32 %p pos16 %p col %p nrm %p st %p\n", ring, ring_kb,
            s_pos32, s_pos16, s_col, s_nrm, s_st);
  mx_printf("vumax: kernels (vertices/iteration, II, cycles/vertex, batch):");
  for (int s = 0; s < NS; s++)
    mx_printf(" %s %u/%u=%u.%u n%u", vx[s].name + 3, vx[s].u, vx[s].ii, vx[s].ii / vx[s].u,
              vx[s].ii * 10 / vx[s].u % 10, max_n(&vx[s]));
  mx_printf("\n");

  if (quick >= 2) {
    if (quick == 2)
      kernels_alone();
    else
      batches_chain();
    MX_TEST_END(0);
    return 0;
  }
  /* part 1 */
  struct mx_shader sh[NS] = {MX_VU_SHADER(VH_FLAT, vh_flat_vu, vh_flat_vu_end),
                             MX_VU_SHADER(VH_GOUR, vh_gour_vu, vh_gour_vu_end),
                             MX_VU_SHADER(VH_TEX, vh_tex_vu, vh_tex_vu_end),
                             MX_VU_SHADER(VH_LIT, vh_lit_vu, vh_lit_vu_end),
                             MX_VU_SHADER(VH_LITTEX, vh_littex_vu, vh_littex_vu_end),
                             MX_VU_SHADER(VH_SCEI, vh_scei_vu, vh_scei_vu_end),
                             MX_VU_SHADER(VH_TEX16, vh_tex16_vu, vh_tex16_vu_end)};
  if (!quick && part1(sh)) {
    MX_TEST_END(1);
    return 1;
  }

  /* part 2 */
  for (u32 i = 0; i < 64 * 64; i++)
    tex[i] = 0x80000000u | (((i ^ i >> 6) & 8) ? 0xC0C0C0u : 0x606060u);
  mx_dcache_writeback(tex, sizeof tex);
  mx_frame_init(ring, ring_kb * 1024, (void *)0x70002000, 0x2000);
  mx_frame_texture(0, mx_gs.tbp, 1, MX_GS_PSM_CT32, 64, 64, tex);
  mx_frame_wait_path3();
  mx_frame_vsync(0);
  mx_printf("vumax: %u patches of %u strip vertices per frame, %u frames each, no vsync\n", PATCHES,
            NV, FRAMES);
  static struct result res[NS][5];
  for (int s = 0; s < NS; s++) {
    if (!(only >> s & 1))
      continue;
    measure(s, C16, 0, 0, &res[s][0]);
    print_row(sc_name[s], "vsm c16 no kick", &res[s][0]);
    measure(s, F32, 0, 0, &res[s][1]);
    print_row(sc_name[s], "vsm f32 no kick", &res[s][1]);
    measure(s, C16, 1, 0, &res[s][2]);
    print_row(sc_name[s], "vsm c16", &res[s][2]);
    measure(s, F32, 1, 0, &res[s][3]);
    print_row(sc_name[s], "vsm f32", &res[s][3]);
    measure(s, F32, 1, &sh[s], &res[s][4]);
    print_row(sc_name[s], "hlsl f32", &res[s][4]);
  }
  mx_printf("vumax summary, cycles/vertex: scenario, floor, vsm c16 no kick / f32 no kick / c16 / "
            "f32, hlsl f32\n");
  for (int s = 0; s < NS; s++) {
    if (!(only >> s & 1))
      continue;
    mx_printf("vumax | %-10s | %u.%u |", sc_name[s], vx[s].ii / vx[s].u, vx[s].ii * 10 / vx[s].u % 10);
    for (int j = 0; j < 5; j++) {
      u32 c100 = (u32)((u64)res[s][j].cycles * 100 / (PATCHES * NV));
      mx_printf(" %u.%02u |", c100 / 100, c100 % 100);
    }
    mx_printf("\n");
  }
  mx_frame_vsync(1);

  /* part 3: one frame of S6 shown and saved; the patches cover most of line
     100 (each patch narrows by 1 / w = 0.89 towards its far edge, which leaves
     a wedge of background between the tiles) */
  u32 last = mx_gs.frames;
  mat4 pc;
  for (int i = 0; i < PATCHES; i++) {
    patch_proj(i, pc);
    vx_uniforms(S_SCEI, C16, scene.p16, pc, &scene_light, uni_f[i]);
  }
  mx_dcache_writeback(uni_f, sizeof uni_f);
  mx_frame_mpg(vx[S_SCEI].code, (u32)(vx[S_SCEI].end - vx[S_SCEI].code), vx[S_SCEI].at);
  frame(S_SCEI, C16, 1, 0, 14);
  u32 d0 = mx_count();
  while (mx_gs.shown < last + 1 && mx_count() - d0 < 300000000u)
    ;
  mx_frame_stop();
  static u32 px[64] MX_ALIGNED(64);
  u32 covered = 0;
  for (u32 x = 0; x < 640; x += 64) {
    mx_gs_download(px, MX_GS_PAGE(mx_gs.fbp[last & 1]), mx_gs.fbw, MX_GS_PSM_CT32, x, 100, 64, 1);
    for (int i = 0; i < 64; i++)
      covered += (px[i] & 0xFFFFFF) != 0x301810;
  }
  mx_printf("vumax: %u of 640 pixels of line 100 drawn\n", covered);
  if (mx_gs_save_tga((int)(last & 1), "host:vumax.tga") == 0)
    mx_printf("vumax: frame saved to vumax.tga\n");
  int f = covered < 520;
  MX_TEST_END(f);
  return f;
}
