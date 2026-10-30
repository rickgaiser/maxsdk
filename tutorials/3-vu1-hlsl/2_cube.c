/* Tutorial 3, step 2: 3D cubes with t3_cube.hlsl. The EE makes one matrix
   a cube (camera.h): its turn, its place, and the projection; VU1 does the
   rest for every vertex. Three cubes, one large in the middle and two that
   circle it, in front of it and behind it: the Z buffer sorts them out. */

#include "camera.h"
#include "t3_cube.h"

#define FRAMES 150
#define NV 36 /* 6 faces of 2 triangles */

extern const u8 t3_cube_vu[], t3_cube_vu_end[];

static u8 ring[16 * 1024] MX_ALIGNED(16 * 1024);
static float pos[NV + VU_T3_CUBE_ALIGN][3] MX_ALIGNED(16);
static u8 col[NV + VU_T3_CUBE_ALIGN][4] MX_ALIGNED(16);

/* the cube from -1 to 1: for each face its corners (bits of x, y, z) as two
   triangles, and its colour; each corner a little darker or lighter, so the
   Gouraud shading shows */
static void make_cube(void) {
  static const u8 faces[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1},
                                 {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}};
  static const u8 rgb[6][3] = {{0xF0, 0x50, 0x50}, {0x50, 0xF0, 0x50}, {0x50, 0x70, 0xF0},
                               {0xF0, 0xE0, 0x50}, {0xE0, 0x60, 0xF0}, {0x50, 0xE0, 0xE0}};
  static const u8 tri[6] = {0, 1, 2, 0, 2, 3};
  static const u8 shade[4] = {0x80, 0x60, 0x40, 0x60};
  for (int f = 0; f < 6; f++)
    for (int k = 0; k < 6; k++) {
      int i = f * 6 + k, corner = faces[f][tri[k]];
      pos[i][0] = corner & 1 ? 1.0f : -1.0f;
      pos[i][1] = corner & 2 ? 1.0f : -1.0f;
      pos[i][2] = corner & 4 ? 1.0f : -1.0f;
      for (int j = 0; j < 3; j++)
        col[i][j] = (u8)(rgb[f][j] * (0x80 + shade[tri[k]]) / 0x100);
      col[i][3] = 0x80;
    }
  mx_dcache_writeback(pos, sizeof pos);
  mx_dcache_writeback(col, sizeof col);
}

int main(void) {
  mx_gs_init(640, 224);
  mx_vu_reset(2);
  mx_frame_init(ring, sizeof ring, (void *)0x70002000, 0x2000);
  const struct mx_shader s = MX_VU_SHADER(T3_CUBE, t3_cube_vu, t3_cube_vu_end);
  make_cube();
  const void *streams[2] = {pos, col};
  const u32 prim = (u32)mx_gs_prim(MX_GS_PRIM_TRIANGLE, 1, 0, 0, 0, 0, 0, 0, 0);
  /* the projection: depths 4 to 16 in front of the eye */
  mat4 proj;
  mat_project(proj, 4, 16);

  float a = 0;
  for (u32 f = 0; f < FRAMES; f++, a += 0.025f) {
    mx_frame_begin(0x10, 0x10, 0x20);
    z_test_on();
    mx_frame_shader_setup(&s);
    for (int k = 0; k < 3; k++) {
      /* the model: turned, scaled (the small ones), moved; 10 in front of
         the eye; the small ones circle it at a radius of 4 */
      mat4 model, mvp;
      mat_turn(model, a * (k ? 2.0f : 1.0f), 0.5f + 0.3f * (float)k);
      float size = k ? 0.6f : 1.6f, t = a + 3.1415927f * (float)k;
      for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
          model[r][c] *= size;
      mat_move(model, k ? 4 * tut_cos(t) : 0, k ? 0.8f * (float)(k * 2 - 3) : 0,
               10 + (k ? 4 * tut_sin(t) : 0));
      mat_mul(mvp, proj, model);
      float uni[VU_T3_CUBE_UNI_QW][4] MX_ALIGNED(16);
      mat_uniform(&uni[VU_T3_CUBE_MVP], mvp);
      mx_frame_shader_draw(&s, prim, uni, streams, NV);
    }
    mx_frame_end();
  }
  tut_screenshot("host:2_cube.tga");
  MX_TEST_END(0);
  return 0;
}
