/* Tutorial 3, step 3: a lit torus with t3_lit.hlsl. The EE makes the torus
   once, its positions and normals as triangles; each frame it turns it and
   gives VU1 the light's direction in the torus's own space: the turn's
   inverse applied to the light (for a turn, the inverse is the transpose). */

#include "camera.h"
#include "t3_lit.h"

#define FRAMES 150
#define RINGS 32 /* around the torus */
#define SIDES 16 /* around its tube */
#define NV (RINGS * SIDES * 6)

extern const u8 t3_lit_vu[], t3_lit_vu_end[];

static u8 ring[16 * 1024] MX_ALIGNED(16 * 1024);
static float pos[NV + VU_T3_LIT_ALIGN][3] MX_ALIGNED(16);
static float nrm[NV + VU_T3_LIT_ALIGN][3] MX_ALIGNED(16);

/* the torus: radius 1 around the y axis, its tube radius 0.4. Point (i, j):
   ring i (around y), side j (around the tube); its normal points away from
   the tube's middle. */
static void make_torus(void) {
  static const u8 quad[6][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 0}, {1, 1}, {0, 1}};
  int n = 0;
  for (int i = 0; i < RINGS; i++)
    for (int j = 0; j < SIDES; j++)
      for (int k = 0; k < 6; k++) {
        float u = 6.2831853f * (float)(i + quad[k][0]) / RINGS;
        float v = 6.2831853f * (float)(j + quad[k][1]) / SIDES;
        float cu = tut_cos(u), su = tut_sin(u), cv = tut_cos(v), sv = tut_sin(v);
        nrm[n][0] = cv * cu, nrm[n][1] = sv, nrm[n][2] = cv * su;
        pos[n][0] = (1 + 0.4f * cv) * cu, pos[n][1] = 0.4f * sv, pos[n][2] = (1 + 0.4f * cv) * su;
        n++;
      }
  mx_dcache_writeback(pos, sizeof pos);
  mx_dcache_writeback(nrm, sizeof nrm);
}

int main(void) {
  mx_gs_init(640, 224);
  mx_vu_reset(2);
  mx_frame_init(ring, sizeof ring, (void *)0x70002000, 0x2000);
  const struct mx_shader s = MX_VU_SHADER(T3_LIT, t3_lit_vu, t3_lit_vu_end);
  make_torus();
  const void *streams[2] = {pos, nrm};
  const u32 prim = (u32)mx_gs_prim(MX_GS_PRIM_TRIANGLE, 1, 0, 0, 0, 0, 0, 0, 0);
  mat4 proj;
  mat_project(proj, 2, 8);
  /* the light: from the upper left, in front (towards the eye: -z) */
  static const float light[3] = {-0.5f, 0.6f, -0.62f};

  float a = 0;
  for (u32 f = 0; f < FRAMES; f++, a += 0.02f) {
    mx_frame_begin(0x08, 0x08, 0x18);
    z_test_on();
    mx_frame_shader_setup(&s);
    mat4 model, mvp;
    mat_turn(model, a, 0.6f + a * 0.7f);
    float uni[VU_T3_LIT_UNI_QW][4] MX_ALIGNED(16);
    /* the light into model space: the turn's transpose (before the move) */
    for (int r = 0; r < 3; r++)
      uni[VU_T3_LIT_LIGHT][r] =
          model[0][r] * light[0] + model[1][r] * light[1] + model[2][r] * light[2];
    uni[VU_T3_LIT_LIGHT][3] = 0;
    for (int r = 0; r < 3; r++)
      for (int c = 0; c < 3; c++)
        model[r][c] *= 1.3f;
    mat_move(model, 0, 0, 5);
    mat_mul(mvp, proj, model);
    mat_uniform(&uni[VU_T3_LIT_MVP], mvp);
    uni[VU_T3_LIT_COLOUR][0] = 230, uni[VU_T3_LIT_COLOUR][1] = 150, uni[VU_T3_LIT_COLOUR][2] = 40;
    uni[VU_T3_LIT_AMBIENT][0] = 20, uni[VU_T3_LIT_AMBIENT][1] = 18, uni[VU_T3_LIT_AMBIENT][2] = 30;
    uni[VU_T3_LIT_AMBIENT][3] = 128;
    mx_frame_shader_draw(&s, prim, uni, streams, NV);
    mx_frame_end();
  }
  tut_screenshot("host:3_light.tga");
  MX_TEST_END(0);
  return 0;
}
