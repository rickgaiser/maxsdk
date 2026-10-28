#ifndef MX_VU0R_H
#define MX_VU0R_H

/* Rendering with the EE and VU0 next to VU1: VU0 micro programs fed through
   VF registers (two register sets: the EE fills one and stores the other's
   results while VU0 runs), their results stored by the EE into GS packets
   of the PATH3 stream (<mx/frame.h>, mx_frame3_*). The EE is busy for the
   whole draw; VU1 renders the VIF1 stream meanwhile.

   SCEI: the math of apps/model/mscei.hlsl (three directional lights
   clamped at 255, textured, perspective correct, the strip restart flag in
   the position's w), the same inputs and uniforms, the same results bit
   for bit (the compiled program's order of operations). */

#include <mx/types.h>

/* VU0 data memory of the uniforms (quadwords), and their count. 0 ..
   MX_VU0R_UNI - 1 are the caller's (apps/model's skinning palette). */
#define MX_VU0R_UNI 240
#define MX_VU0R_UNI_QW 13
/* Vertices of a GIF packet at most (a strip is cut into packets that repeat
   two vertices): 3 quadwords each, fewer if a PATH3 half has less room
   (mx_frame3_room). */
#define MX_VU0R_BATCH 80

/* Upload the micro programs to VU0 micro memory at addr (instructions; they
   take MX_VU0R_CODE_INSNS). They use VF registers vf1 .. vf30 (not vf31)
   and I. Returns mx_vu_load's result, -1 if they do not fit (VU0 micro
   memory is 512 instructions). */
#define MX_VU0R_CODE_INSNS 80
int mx_vu0r_load(u32 addr);

/* One strip of n vertices (a run of strips restarted by ADC) into the PATH3
   stream: position f32x4 (w 2056.0 where a strip restarts, else 0), normal
   s8x4 (length 127), texture coordinates s16x2 (4.12), 16, 4 and 4 bytes
   apart; uni as mscei's (MX_VU0R_UNI_QW quadwords: MVP columns, a[3],
   e[3], amb, prm, scl). prim: the PRIM of each packet (a TRISTRIP with
   CTXT 1, the PATH3 stream's context). Waits for a running VU0 program. */
void mx_vu0r_scei(const float (*uni)[4], u32 prim, const void *pos, const void *nrm, const void *uv,
                  u32 n);

#endif
