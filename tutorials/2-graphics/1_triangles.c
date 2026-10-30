/* Tutorial 2, step 1: triangles, flat and Gouraud shaded, and a fan.

   Tutorial 1 wrote each register with an A+D pair. For vertices, PACKED mode
   is shorter: the GIF tag lists the registers once (here RGBAQ, then XYZ2)
   and sets PRIM itself; each vertex is then one quadword per register. NLOOP
   in the tag is the number of vertices. */

#include "tut.h"

#define FRAMES 120
#define FAN 24 /* the fan's outer vertices */

static u8 ring[16 * 1024] MX_ALIGNED(16 * 1024);

/* 0 .. 255 as t goes around */
static u32 wheel(float t) { return (u32)(127.5f + 127 * tut_sin(t)); }

struct vtx {
  float x, y; /* pixel, line */
  u32 r, g, b;
};

/* n vertices as primitive prim, in one GIF packet */
static void draw(u64 prim, const struct vtx *v, u32 n) {
  u64 *c = mx_frame_direct(1 + 2 * n);
  c = mx_e_giftag(c, n, 1, (s32)prim, MX_GIF_FLG_PACKED, 2,
                  mx_gif_regs2(MX_GIF_REG_RGBAQ, MX_GIF_REG_XYZ2));
  for (u32 i = 0; i < n; i++) {
    c = mx_e_rgbaq(c, v[i].r, v[i].g, v[i].b, 0x80);
    /* 12.4 fixed point: a vertex can be placed at a 16th of a pixel */
    c = mx_e_xyz2(c, mx_gs_px(0) + (u32)(v[i].x * 16), mx_gs_py(0) + (u32)(v[i].y * 16), 0, 0);
  }
  mx_frame_commit(c);
}

int main(void) {
  mx_gs_init(640, 224);
  mx_frame_init(ring, sizeof ring, (void *)0x70002000, 0x2000);

  /* PRIM: the primitive, IIP (0 flat: the last vertex's colour, 1 Gouraud:
     the colours blended across), TME, FGE, ABE, AA1, FST, CTXT, FIX */
  const u64 flat = mx_gs_prim(MX_GS_PRIM_TRIANGLE, 0, 0, 0, 0, 0, 0, 0, 0);
  const u64 gouraud = mx_gs_prim(MX_GS_PRIM_TRIANGLE, 1, 0, 0, 0, 0, 0, 0, 0);
  const u64 fan = mx_gs_prim(MX_GS_PRIM_TRIFAN, 1, 0, 0, 0, 0, 0, 0, 0);
  const struct vtx tri[3] = {{100, 40, 0xFF, 0, 0}, {180, 180, 0, 0xFF, 0}, {20, 180, 0, 0, 0xFF}};

  float a = 0;
  for (u32 f = 0; f < FRAMES; f++, a += 0.02f) {
    mx_frame_begin(0x10, 0x10, 0x20);
    /* the same triangle twice: flat, then Gouraud */
    draw(flat, tri, 3);
    struct vtx t2[3];
    for (int i = 0; i < 3; i++)
      t2[i] = tri[i], t2[i].x += 200;
    draw(gouraud, t2, 3);
    /* a fan: the centre, then FAN + 1 points around it (the last is the
       first again); each new point makes a triangle with the centre and the
       point before. Its colours go around the colour wheel, and it turns by
       a each frame. */
    struct vtx v[FAN + 2] = {{530, 110, 0xFF, 0xFF, 0xFF}};
    for (int i = 0; i <= FAN; i++) {
      float t = 6.2831853f * (float)i / FAN;
      v[i + 1].x = 530 + 90 * tut_cos(a + t), v[i + 1].y = 110 + 45 * tut_sin(a + t);
      v[i + 1].r = wheel(t), v[i + 1].g = wheel(t + 2.0943951f), v[i + 1].b = wheel(t - 2.0943951f);
    }
    draw(fan, v, FAN + 2);
    mx_frame_end();
  }
  tut_screenshot("host:1_triangles.tga");
  MX_TEST_END(0);
  return 0;
}
