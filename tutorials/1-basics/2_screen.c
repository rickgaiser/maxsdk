/* Tutorial 1, step 2: the screen and the frame loop.

   mx_gs_init sets the video mode and the GS's framebuffers; the frame
   pipeline (mx_frame_init) takes what a frame draws to the GS. Each turn of
   the loop is one frame: mx_frame_begin waits for a free framebuffer and
   clears it, mx_frame_end sends the frame off. The vblank interrupt shows
   each frame when it is drawn: 60 frames a second at most. */

#include "tut.h"

#define FRAMES 180

/* the ring that the frame pipeline's DMA reads from: a power of two in
   size, aligned to its size */
static u8 ring[16 * 1024] MX_ALIGNED(16 * 1024);

/* 0 .. 255 .. 0 as t goes 0 .. 1 .. 2 */
static u32 wave(u32 t, u32 period) {
  u32 p = t % period * 512 / period;
  return p < 256 ? p : 511 - p;
}

int main(void) {
  /* NTSC, interlaced, 640 x 224: two framebuffers and a Z buffer */
  mx_gs_init(640, 224);
  /* the pipeline: the ring, and 8 KB of scratchpad (0x70000000) where the
     EE builds the frame */
  mx_frame_init(ring, sizeof ring, (void *)0x70002000, 0x2000);

  u32 v0 = mx_gs.vsync;
  for (u32 f = 0; f < FRAMES; f++) {
    /* a colour that changes every frame */
    mx_frame_begin(wave(f, 180), wave(f + 60, 180), wave(f + 120, 180));
    mx_frame_end();
  }
  /* mx_gs counts the vblanks and the frames shown; missed: vblanks where the
     next frame was not ready. Wait for the last frame to be shown first. */
  while (mx_gs.shown != FRAMES)
    ;
  mx_printf("%u frames shown in %u vblanks, %u missed\n", mx_gs.shown, mx_gs.vsync - v0,
            mx_gs.missed);
  tut_screenshot("host:2_screen.tga");
  MX_TEST_END(0);
  return 0;
}
