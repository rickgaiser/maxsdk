/* camera.h: the matrices of tutorial 3's 3D steps. A matrix is mat4 m,
   m[row][column], and transforms a column vector: p' = m p. The vertex
   programs take it column by column (HLSL's cbuffer: one quadword a
   column): mat_uniform. */

#ifndef CAMERA_H
#define CAMERA_H

#include "tut.h"

typedef float mat4[4][4];

/* r = a b (r may not be a or b) */
static void mat_mul(mat4 r, const mat4 a, const mat4 b) {
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 4; j++)
      r[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
}

/* turned by angle a (radians) around the y axis, then by b around x */
static void mat_turn(mat4 m, float a, float b) {
  float ca = tut_cos(a), sa = tut_sin(a), cb = tut_cos(b), sb = tut_sin(b);
  mat4 y = {{ca, 0, sa, 0}, {0, 1, 0, 0}, {-sa, 0, ca, 0}, {0, 0, 0, 1}};
  mat4 x = {{1, 0, 0, 0}, {0, cb, -sb, 0}, {0, sb, cb, 0}, {0, 0, 0, 1}};
  mat_mul(m, x, y);
}

/* m, then moved by (x, y, z) */
static void mat_move(mat4 m, float x, float y, float z) {
  m[0][3] += x, m[1][3] += y, m[2][3] += z;
}

/* The projection: a point (x, y, z) in front of the eye (y up, z the depth)
   goes to the GS's coordinates, (2048 + FX x / z, 2048 - FY y / z): 2048 is
   the middle of the screen. FX = 457 pixels gives a view 70 degrees wide
   over 640 pixels; FY = FX * 0.467, as the screen is 4:3 and a line of the
   224 is twice as tall as a pixel is wide. Its Z is ZA / z + ZB: Z_FAR at
   depth far, Z_NEAR at depth near, more where it is nearer (for the Z test
   GEQUAL). The program writes Z with ftoi4 (16 times it), into 24 bits:
   Z_NEAR at most 2^20. The GS divides by w = z: p' / p'.w. */
#define FX 457.0f
#define FY (457.0f * 0.467f)
#define Z_NEAR 1000000.0f
#define Z_FAR 10000.0f
static void mat_project(mat4 m, float near, float far) {
  float za = (Z_NEAR - Z_FAR) / (1 / near - 1 / far), zb = Z_NEAR - za / near;
  mat4 p = {{FX, 0, 2048, 0}, {0, -FY, 2048, 0}, {0, 0, zb, za}, {0, 0, 1, 0}};
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++)
      m[r][c] = p[r][c];
}

/* m into four uniform quadwords at u, a column each */
static void mat_uniform(float (*u)[4], const mat4 m) {
  for (int c = 0; c < 4; c++)
    for (int r = 0; r < 4; r++)
      u[c][r] = m[r][c];
}

/* Draw with the Z test: a pixel only where it is at least as near
   (ZTST GEQUAL). mx_frame_begin clears Z to 0, the farthest. */
static void z_test_on(void) {
  u64 *c = mx_frame_direct(1 + 1);
  mx_frame_commit(
      mx_e_ad(mx_e_ad_tag(c, 1, 1), MX_GS_REG_TEST_1, mx_gs_test(0, 0, 0, 0, 0, 0, 1, 2)));
}

#endif
