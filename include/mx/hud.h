#ifndef MX_HUD_H
#define MX_HUD_H

/* Text for on-screen counters: a 5x7 font (digits, A-Z and " .:%/-") in a
   256 x 8 texture, drawn as PATH2 sprites on the frame pipeline
   (<mx/frame.h>), in lines of a 224-line screen. */

#include <mx/types.h>

/* Make the font and upload it to tbp by PATH3 (texture slot 0) on a frame
   pipeline that has been started; lines: the height of the screen. */
void mx_hud_init(u32 tbp, u32 lines);
/* Text at (x, y): pixels, and lines of a 224-line screen, in colour rgb
   (0xRRGGBB, 0x80 full) and scale times as big. Characters not in the font
   are spaces. */
void mx_hud_text(u32 x, u32 y, const char *s, u32 rgb, u32 scale);
/* The width in pixels of text s at scale. */
u32 mx_hud_width(const char *s, u32 scale);
/* A rectangle at (x, y), w x h (pixels, and lines of a 224-line screen),
   blended over what is drawn: rgba (0xAARRGGBB, alpha 0x80 opaque). */
void mx_hud_box(u32 x, u32 y, u32 w, u32 h, u32 rgba);

/* Strings: each appends to d and returns the new end (NUL written). */
char *mx_hud_cat(char *d, const char *s);
/* v in decimal */
char *mx_hud_u(char *d, u32 v);
/* v with one decimal of v / div: "12.3" */
char *mx_hud_u1(char *d, u64 v, u64 div);

#endif
