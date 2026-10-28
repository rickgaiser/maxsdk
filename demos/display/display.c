/* The video modes and the two read circuits (<mx/video.h>, <mx/display.h>),
   to look at on a screen.

   Each mode has a full-resolution overlay buffer (CT16S: its 1-bit alpha
   says where it covers) with a border at the edge of the picture, a centre
   cross and the mode's name, and two animated buffers at half the
   resolution (CT16S), which the circuits magnify 2x. The scenes:

     OVERLAY    circuit 1 = overlay over circuit 2 = animation, alpha from
                the overlay's pixels
     CROSSFADE  the same at a constant alpha going 0 -> 1 -> 0
     FADE       the animation on circuit 1 over BGCOLOR (black), alpha
                going 0 -> 1 -> 0
     SMOOTH     the animation on both circuits, circuit 1 half a pixel
                right, 1:1 (mx_gs_smooth)
     LOW        the animation alone
     FULL       the overlay alone, unmagnified

   Pad: left/right the mode, square the scene, the d-pad up/down and L1/R1
   move the picture (DY, DX: the values are shown, so a mode's offsets can
   be written down), start exits. Until a button is pressed the demo shows
   every mode for a second, then every scene, and exits. Arguments: m<n>
   start at mode n (0 = the first of the table), s<n> n seconds a step. */

#include <mx/mx.h>
#include <mx/pad.h>
#include "hud.h"

struct demo_mode {
  const char *name;
  const struct mx_video_mode *m;
  int ffmd;
  u32 w, h; /* the overlay buffer: full resolution */
};

static const struct demo_mode modes[] = {
    {"NTSC 480I FIELD", &mx_video_ntsc, 0, 640, 448},
    {"NTSC 480I FRAME", &mx_video_ntsc, 1, 640, 224},
    {"NTSC 240P", &mx_video_ntsc_240p, 0, 640, 224},
    {"PAL 576I FIELD", &mx_video_pal, 0, 640, 512},
    {"PAL 288P", &mx_video_pal_288p, 0, 640, 256},
    {"480P", &mx_video_480p, 0, 640, 480},
    {"576P", &mx_video_576p, 0, 640, 576},
    {"720P", &mx_video_720p, 0, 1280, 720},
    {"1080I FRAME", &mx_video_1080i, 1, 960, 540},
    {"VGA 640X480", &mx_video_vga_640, 0, 640, 480},
    {"VGA 800X600", &mx_video_vga_800, 0, 800, 600},
    {"VGA 1024X768", &mx_video_vga_1024, 0, 1024, 768},
    {"VGA 1280X1024", &mx_video_vga_1280, 0, 1280, 1024},
};
#define NMODES (int)(sizeof modes / sizeof *modes)

enum { OVERLAY, CROSSFADE, FADE, SMOOTH, LOW, FULL, NSCENES };
static const char *const scenes[] = {"OVERLAY", "CROSSFADE", "FADE", "SMOOTH", "LOW", "FULL"};

static u8 ring[64 * 1024] MX_ALIGNED(64 * 1024);
static struct mx_rt hud, lo[2];
static struct mx_display disp[2];
static int mode = -1, scene, running;
static s32 dx, dy;

/* text and boxes of hud.h go to a target of this size */
static void hud_target(const struct mx_rt *t) {
  mx_gs.w = t->w;
  mx_gs.h = t->h;
}

/* the state the GS reset (mx_video_set) cleared, context 1 */
static void state(void) {
  u64 *c = mx_frame_direct(6);
  c = mx_e_ad_tag(c, 5, 1);
  c = mx_e_ad(c, MX_GS_REG_TEST_1, mx_gs_test(0, 0, 0, 0, 0, 0, 1, 1));
  c = mx_e_ad(c, MX_GS_REG_PRMODECONT, 1);
  c = mx_e_ad(c, MX_GS_REG_COLCLAMP, 1);
  c = mx_e_ad(c, MX_GS_REG_DTHE, 0);
  mx_frame_commit(mx_e_ad(c, MX_GS_REG_TEXA, 0x80ull << 32));
}

/* a flat sprite (rgba 0xAARRGGBB, alpha < 0x80: transparent in CT16S) */
static void box(const struct mx_rt *t, u32 x0, u32 y0, u32 x1, u32 y1, u32 rgba) {
  u64 *c = mx_frame_direct(5);
  c = mx_e_ad_tag(c, 4, 1);
  c = mx_e_ad(c, MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 0, 0, 0, 0, 0, 0, 0));
  c = mx_e_ad(c, MX_GS_REG_RGBAQ,
              mx_gs_rgbaq(rgba >> 16 & 0xFF, rgba >> 8 & 0xFF, rgba & 0xFF, rgba >> 24, 1.0f));
  c = mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(mx_rt_px(t, x0), mx_rt_py(t, y0), 0));
  mx_frame_commit(mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(mx_rt_px(t, x1), mx_rt_py(t, y1), 0)));
}

static void label(char *s) {
  char *p = hud_cat(s, modes[mode].name);
  p = hud_cat(p, "  ");
  p = hud_cat(p, scenes[scene]);
  p = hud_cat(p, "  DX ");
  if (dx < 0)
    p = hud_cat(p, "-");
  p = hud_u(p, (u32)(dx < 0 ? -dx : dx));
  p = hud_cat(p, " DY ");
  if (dy < 0)
    p = hud_cat(p, "-");
  hud_u(p, (u32)(dy < 0 ? -dy : dy));
}

/* the overlay: transparent, a 2-pixel border at the edge, a centre cross */
static void draw_hud(void) {
  const struct mx_rt *t = &hud;
  u32 w = t->w, h = t->h;
  char s[96];
  mx_frame_target(t, 0);
  hud_target(t);
  state();
  box(t, 0, 0, w, h, 0x00000000);
  box(t, 0, 0, w, 2, 0x80FFFFFF);
  box(t, 0, h - 2, w, h, 0x80FFFFFF);
  box(t, 0, 0, 2, h, 0x80FFFFFF);
  box(t, w - 2, 0, w, h, 0x80FFFFFF);
  box(t, w / 2 - 1, h / 2 - 20, w / 2 + 1, h / 2 + 20, 0x80FFFF00);
  box(t, w / 2 - 20, h / 2 - 1, w / 2 + 20, h / 2 + 1, 0x80FFFF00);
  label(s);
  hud_text(16, 16, s, 0xFFFFFF, 1);
  char *p = hud_u(s, w);
  p = hud_cat(p, "X");
  hud_u(p, h);
  hud_text(16, 28, s, 0xFFFFFF, 1);
}

/* the animation: bars moving across, and the label */
static void draw_lo(const struct mx_rt *t, u32 k) {
  u32 w = t->w, h = t->h;
  char s[96];
  mx_frame_target(t, 0);
  hud_target(t);
  state();
  box(t, 0, 0, w, h, 0x80102040);
  for (u32 i = 0; i < 4; i++) {
    u32 x = (k * (2 + i) + i * w / 4) % w;
    box(t, x, h / 8 + i * h / 5, x + w / 8 > w ? w : x + w / 8, h / 8 + i * h / 5 + h / 8,
        0x80000000 | (0xFF0000 >> (8 * (i % 3))) | 0x404040);
  }
  label(s);
  hud_text(8, h - 16, s, 0xFFFFFF, 1);
}

static u8 tri(u32 k) { /* 0 -> 0xFF -> 0 every 128 frames */
  u32 v = k % 128;
  return (u8)(v < 64 ? v * 4 : (127 - v) * 4);
}

/* what frame k shows */
static void display(struct mx_display *d, u32 k) {
  const struct mx_rt *a = &lo[k & 1];
  *d = (struct mx_display){0};
  switch (scene) {
  case OVERLAY:
  case CROSSFADE:
    mx_circuit_rt(&d->c[0], &hud);
    mx_circuit_rt(&d->c[1], a);
    d->mmod = scene == CROSSFADE;
    d->alp = tri(k);
    break;
  case FADE:
    mx_circuit_rt(&d->c[0], a);
    d->mmod = 1;
    d->slbg = 1;
    d->alp = tri(k);
    break;
  case SMOOTH:
    mx_circuit_rt(&d->c[0], a);
    mx_circuit_rt(&d->c[1], a);
    d->c[0].dx = (s32)mx_circuit_magh(&mx_video, &d->c[1]) / 2;
    d->mmod = 1;
    d->alp = 0x80;
    break;
  case LOW:
    mx_circuit_rt(&d->c[1], a);
    d->mmod = 1;
    break;
  case FULL:
    mx_circuit_rt(&d->c[1], &hud);
    d->mmod = 1;
    break;
  }
  for (int i = 0; i < 2; i++)
    d->c[i].dx += dx, d->c[i].dy += dy;
}

static void wait_shown(void) {
  u32 t0 = mx_count();
  while (mx_gs.shown < mx_gs.frames && mx_count() - t0 < 300000000u)
    ;
}

static void set_mode(int i) {
  const struct demo_mode *dm = &modes[i];
  if (running) {
    wait_shown();
    mx_printf("display: stop (shown %u of %u)\n", mx_gs.shown, mx_gs.frames);
    mx_frame_stop();
  }
  struct mx_video_mode m = *dm->m;
  m.ffmd = (u8)dm->ffmd;
  mx_printf("display: SetGsCrt %u %u %u\n", m.interlace, m.code, m.ffmd);
  if (mx_video_set(&m) < 0) {
    /* no vblank: this console does not have the mode */
    mx_printf("display: %s: no vblank, not on this console\n", dm->name);
    running = 0;
    set_mode(0);
    return;
  }
  mx_vram_reset();
  mx_rt_init(&hud, mx_vram_alloc(mx_rt_pages(dm->w, dm->h, MX_GS_PSM_CT16S)), dm->w, dm->h,
             MX_GS_PSM_CT16S, 0, MX_RT_NOZ);
  for (int j = 0; j < 2; j++) {
    u32 w = dm->w / 2, h = dm->h / 2;
    mx_rt_init(&lo[j], mx_vram_alloc(mx_rt_pages(w, h, MX_GS_PSM_CT16S)), w, h, MX_GS_PSM_CT16S, 0,
               MX_RT_NOZ);
  }
  mx_printf("display: %s, %u pages\n", dm->name, mx_gs.vram);
  mode = i;
  dx = dy = 0;
  mx_gs_displays(disp, 2);
  mx_frame_init(ring, sizeof ring, (void *)0x70002000, 0x2000);
  running = 1;
  hud_init(mx_gs.tbp, 224);
}

int main(int argc, char **argv) {
  int first = 0, secs = 0;
  for (int a = 1; a < argc; a++) {
    int *v = argv[a][0] == 'm' ? &first : argv[a][0] == 's' ? &secs : 0;
    if (v)
      for (const char *p = argv[a] + 1; *p >= '0' && *p <= '9'; p++)
        *v = *v * 10 + (*p - '0');
  }
  u32 dwell = 60 * (u32)(secs ? secs : 1);
  if (first >= NMODES)
    first = 0;
  mx_gs_init_video(&mx_video_ntsc);
  set_mode(first);
  int pad = mx_pad_init() >= 0, manual = 0;
  /* until a button is pressed: every mode, then every scene, a second each */
  u32 step = (u32)first, nsteps = NMODES + NSCENES, at = 0;
  for (u32 k = mx_gs.frames;; k = mx_gs.frames) {
    u32 b = pad ? mx_pad_pressed(0) : 0;
    if (b & MX_PAD_START)
      break;
    manual |= b != 0;
    if (!manual) {
      if (k - at >= dwell) {
        at = k;
        if (++step == nsteps)
          break;
        if (step < (u32)NMODES)
          set_mode((int)step);
        else {
          if (step == (u32)NMODES)
            set_mode(0);
          scene = (int)(step - NMODES);
        }
        k = mx_gs.frames;
      }
    }
    if (b & (MX_PAD_LEFT | MX_PAD_RIGHT))
      set_mode((mode + (b & MX_PAD_RIGHT ? 1 : NMODES - 1)) % NMODES), k = mx_gs.frames;
    if (b & MX_PAD_SQUARE)
      scene = (scene + 1) % NSCENES;
    dy += b & MX_PAD_DOWN ? 2 : b & MX_PAD_UP ? -2 : 0;
    dx += b & MX_PAD_R1 ? 4 : b & MX_PAD_L1 ? -4 : 0;

    mx_frame_begin_n();
    draw_lo(&lo[k & 1], k);
    draw_hud();
    display(&disp[k & 1], k);
    mx_frame_end();
  }
  wait_shown();
  mx_frame_stop();
  mx_printf("display: done\n");
  MX_TEST_END(0);
  return 0;
}
