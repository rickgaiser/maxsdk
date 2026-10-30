/* Tutorial 3, step 1: drawing with a vertex program (t3_move.hlsl).

   The Makefile compiles t3_move.hlsl into VU1 microcode (t3_move_vu ..
   t3_move_vu_end) and a header, t3_move.h, with its layout: where its
   uniforms are (VU_T3_MOVE_TURN, VU_T3_MOVE_OFFSET), how many vertices a
   batch takes, and its inputs' formats. MX_VU_SHADER makes a struct
   mx_shader of it.

   A draw (mx_frame_shader_draw) sends the uniforms once and then the
   vertices in batches: VU1 transforms one batch while the next is unpacked
   into its memory, and the GS draws what VU1 sends it. Each input is a
   stream of its own in main memory. Here one pinwheel is drawn eight times:
   eight draws of the same vertices, each with its own uniforms. */

#include "t3_move.h"
#include "tut.h"

#define FRAMES 120
#define POINTS 5
#define NV (POINTS * 3) /* a pinwheel: a triangle for each blade */

extern const u8 t3_move_vu[], t3_move_vu_end[];

static u8 ring[16 * 1024] MX_ALIGNED(16 * 1024);
/* the inputs: positions and colours, with room after the last vertex (a
   batch reads whole groups of VU_T3_MOVE_ALIGN vertices) */
static float pos[NV + VU_T3_MOVE_ALIGN][3] MX_ALIGNED(16);
static u8 col[NV + VU_T3_MOVE_ALIGN][4] MX_ALIGNED(16);

int main(void) {
  mx_gs_init(640, 224);
  mx_vu_reset(2); /* VU1 */
  mx_frame_init(ring, sizeof ring, (void *)0x70002000, 0x2000);
  const struct mx_shader s = MX_VU_SHADER(T3_MOVE, t3_move_vu, t3_move_vu_end);

  /* the pinwheel: each blade a triangle from the centre (white) to an inner
     corner (white) and the tip (the blade's colour) */
  static const u8 tip[POINTS][3] = {{0xFF, 0x40, 0x40},
                                    {0xFF, 0xC0, 0x20},
                                    {0x40, 0xE0, 0x40},
                                    {0x40, 0x80, 0xFF},
                                    {0xC0, 0x40, 0xFF}};
  for (int i = 0; i < POINTS; i++) {
    float a = 6.2831853f * (float)i / POINTS;
    float *c = pos[3 * i], *in = pos[3 * i + 1], *t = pos[3 * i + 2];
    c[0] = 0, c[1] = 0, c[2] = 0;
    in[0] = 0.4f * tut_sin(a - 0.6f), in[1] = -0.4f * tut_cos(a - 0.6f), in[2] = 0;
    t[0] = tut_sin(a), t[1] = -tut_cos(a), t[2] = 0;
    for (int k = 0; k < 3; k++) {
      u8 *rgba = col[3 * i + k];
      for (int j = 0; j < 3; j++)
        rgba[j] = k == 2 ? tip[i][j] : 0xFF;
      rgba[3] = 0x80;
    }
  }
  /* VU1 reads them by DMA, from memory, not from the data cache */
  mx_dcache_writeback(pos, sizeof pos);
  mx_dcache_writeback(col, sizeof col);
  const void *streams[2] = {pos, col};
  const u32 prim = (u32)mx_gs_prim(MX_GS_PRIM_TRIANGLE, 1, 0, 0, 0, 0, 0, 0, 0);

  float a = 0;
  for (u32 f = 0; f < FRAMES; f++, a += 0.03f) {
    mx_frame_begin(0x10, 0x10, 0x28);
    /* the program into VU1's micro memory (once: then it is resident) */
    mx_frame_shader_setup(&s);
    for (int k = 0; k < 8; k++) {
      /* the uniforms, one quadword (four floats) each, at the places the
         header gives; they are copied into the frame */
      float uni[VU_T3_MOVE_UNI_QW][4] MX_ALIGNED(16);
      float t = a * (k & 1 ? -1.0f : 1.0f) + 0.4f * (float)k;
      uni[VU_T3_MOVE_TURN][0] = tut_cos(t), uni[VU_T3_MOVE_TURN][1] = tut_sin(t);
      uni[VU_T3_MOVE_TURN][2] = 40 + 6 * (float)k;
      /* GS coordinates: the screen's centre is at (2048, 2048) */
      uni[VU_T3_MOVE_OFFSET][0] = (float)(2048 - 320 + 70 + 70 * k);
      uni[VU_T3_MOVE_OFFSET][1] = 2048 + 50 * tut_sin(a * 2 + (float)k);
      mx_frame_shader_draw(&s, prim, uni, streams, NV);
    }
    mx_frame_end();
  }
  tut_screenshot("host:1_shader.tga");
  MX_TEST_END(0);
  return 0;
}
