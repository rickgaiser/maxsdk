/* tut.h: what the tutorial programs share: a picture of the screen. */

#ifndef TUT_H
#define TUT_H

#include <mx/mx.h>

/* sin and cos (there is no libm): the angle brought into -pi .. pi, then a
   short series, good to about 1e-6. */
static inline float tut_sin(float a) {
  while (a > 3.14159265f)
    a -= 6.28318531f;
  while (a < -3.14159265f)
    a += 6.28318531f;
  float a2 = a * a;
  return a * (1 - a2 / 6 * (1 - a2 / 20 * (1 - a2 / 42 * (1 - a2 / 72 * (1 - a2 / 110)))));
}
static inline float tut_cos(float a) { return tut_sin(a + 1.57079633f); }

/* Save the last frame as a TGA file through the host file system, e.g.
   "host:shot.tga" (the directory of the ELF in PCSX2, ps2client's working
   directory on a PS2). Waits until the frame is on the screen, then stops
   the frame pipeline: call it after the last frame. Returns 0, or -1. */
static inline int tut_screenshot(const char *path) {
  u32 t0 = mx_count();
  while (mx_gs.shown != mx_gs.frames && mx_count() - t0 < MX_EE_HZ)
    ;
  mx_frame_stop();
  return mx_gs_save_tga((int)((mx_gs.frames - 1) & 1), path);
}

#endif
