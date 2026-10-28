#ifndef MX_FRAME_INT_H
#define MX_FRAME_INT_H

/* Between frame.c (the VIF1 stream) and frame3.c (the PATH3 stream). */

#include <mx/dma.h>

/* Everything built so far in the VIF1 stream goes to VIF1 now (without the
   idle work of mx_frame_idle: the PATH3 stream's caller may be using VU0). */
void mx__frame_kick(void);
/* The texture uploads' events of the GIF queue are void (it starts again). */
void mx__frame_tex_forget(void);
/* A wait of the streams has lasted a second: where they stand (once). */
void mx__frame_stalled(const char *what);
/* The VIF1 stream is in chain mode. */
int mx__frame_chain(void);

/* Set by mx_frame3_init while the PATH3 stream is on: */
/* at the end of a frame, before its FINISH (the join) */
extern void (*mx__frame3_end)(void);
/* in mx_frame_stop, after VIF1 has drained */
extern void (*mx__frame3_stop)(void);
/* the GIF queue (texture uploads go through it) */
extern struct mx_dmaq *mx__frame3_q;
/* its part of mx__frame_stalled's report */
extern void (*mx__frame3_report)(void);

#endif
