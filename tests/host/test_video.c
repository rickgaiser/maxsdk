/* Video modes, render targets and displays (<mx/video.h>, <mx/rt.h>,
   <mx/display.h>): the register values, against the ones gs.c wrote before
   (NTSC/PAL 640 wide) and the fitting rules for the other modes. */

#include <mx/bits.h>
#include <mx/display.h>
#include <mx/hw/regs.h>
#include "test.h"

static u64 r[MX_DISP_NREGS];

/* the DISPLAY value gs.c wrote for NTSC/PAL (640 wide) */
static u64 old_display(u32 dx, u32 dy, u32 h) {
  u32 full = h > 256;
  return MX_SET(GS_DISPLAY2_DX, dx) | MX_SET(GS_DISPLAY2_DY, dy) | MX_SET(GS_DISPLAY2_MAGH, 3) |
         MX_SET(GS_DISPLAY2_DW, 0x9FF) | MX_SET(GS_DISPLAY2_DH, (full ? h : h << 1) - 1);
}

static void same_as_before(struct mx_video_mode m, u32 h, u32 dx, u32 dy) {
  struct mx_rt t;
  struct mx_display d;
  m.ffmd = h <= 256;
  mx_rt_init(&t, 70, 640, h, MX_GS_PSM_CT32, 140, MX_GS_PSM_Z24);
  mx_display_init(&d, &t);
  mx_display_regs(&m, &d, r);
  CHECK_EQ(r[MX_DISP_PMODE], 0x66);
  CHECK_EQ(r[MX_DISP_DISPFB2], MX_SET(GS_DISPFB2_FBP, 70) | MX_SET(GS_DISPFB2_FBW, 10));
  CHECK_EQ(r[MX_DISP_DISPLAY2], old_display(dx, dy, h));
  CHECK_EQ(r[MX_DISP_DISPFB1], 0);
  CHECK_EQ(r[MX_DISP_DISPLAY1], 0);
  CHECK_EQ(r[MX_DISP_BGCOLOR], 0);

  /* mx_gs_smooth: circuit 1 the same, 2 VCK (half a pixel) to the right */
  d.c[0] = d.c[1];
  d.c[0].dx = 2;
  d.amod = 0;
  d.alp = 0x80;
  mx_display_regs(&m, &d, r);
  CHECK_EQ(r[MX_DISP_PMODE], MX_SET(GS_PMODE_EN1, 1) | MX_SET(GS_PMODE_EN2, 1) |
                                 MX_SET(GS_PMODE_CRTMD, 1) | MX_SET(GS_PMODE_MMOD, 1) |
                                 MX_SET(GS_PMODE_ALP, 0x80));
  CHECK_EQ(r[MX_DISP_DISPLAY1], old_display(dx + 2, dy, h));
  CHECK_EQ(r[MX_DISP_DISPFB1], r[MX_DISP_DISPFB2]);

  /* MX_SMOOTH_DOWN: circuit 1 the same, reading from the buffer's line 1 */
  d.c[0] = d.c[1];
  d.c[0].dby = 1;
  mx_display_regs(&m, &d, r);
  CHECK_EQ(r[MX_DISP_DISPLAY1], r[MX_DISP_DISPLAY2]);
  CHECK_EQ(r[MX_DISP_DISPFB1], r[MX_DISP_DISPFB2] | MX_SET(GS_DISPFB1_DBY, 1));
}

/* circuit 2 shows a w x h CT32 buffer in mode m */
static u64 disp(const struct mx_video_mode *m, u32 w, u32 h) {
  struct mx_rt t;
  struct mx_display d;
  mx_rt_init(&t, 0, w, h, MX_GS_PSM_CT32, 0, MX_RT_NOZ);
  mx_display_init(&d, &t);
  mx_display_regs(m, &d, r);
  return r[MX_DISP_DISPLAY2];
}

static void check_display(u64 v, u32 dx, u32 dy, u32 magh, u32 magv, u32 dw, u32 dh) {
  CHECK_EQ(MX_GET(GS_DISPLAY2_DX, v), dx);
  CHECK_EQ(MX_GET(GS_DISPLAY2_DY, v), dy);
  CHECK_EQ(MX_GET(GS_DISPLAY2_MAGH, v), magh - 1);
  CHECK_EQ(MX_GET(GS_DISPLAY2_MAGV, v), magv - 1);
  CHECK_EQ(MX_GET(GS_DISPLAY2_DW, v), dw - 1);
  CHECK_EQ(MX_GET(GS_DISPLAY2_DH, v), dh - 1);
}

static void fitting(void) {
  struct mx_video_mode m;
  /* 720p, 640 x 360 doubled both ways */
  check_display(disp(&mx_video_720p, 640, 360), 306, 24, 2, 2, 1280, 720);
  /* 1080i FRAME mode, 960 x 540: each line in both fields */
  m = mx_video_1080i;
  m.ffmd = 1;
  check_display(disp(&m, 960, 540), 236, 38, 2, 1, 1920, 1080);
  /* 1080i FIELD mode, 1920 x 1080 */
  check_display(disp(&mx_video_1080i, 1920, 1080), 236, 38, 1, 1, 1920, 1080);
  /* 240p */
  check_display(disp(&mx_video_ntsc_240p, 640, 224), 0x27C, 25, 4, 1, 2560, 224);
  /* 480p, 640 x 448: centred, 16 lines down */
  check_display(disp(&mx_video_480p, 640, 448), 232 + 80, 35 + 16, 2, 1, 1280, 448);
  /* NTSC: a width that does not divide 2560 is centred (4 VCK a pixel) */
  check_display(disp(&mx_video_ntsc, 600, 448), 0x27C + 80, 50, 4, 1, 2400, 448);
  /* interlaced: centred by whole frames (even lines) */
  m = mx_video_ntsc;
  m.ffmd = 1;
  check_display(disp(&m, 640, 222), 0x27C, 52, 4, 1, 2560, 444);
  check_display(disp(&m, 640, 223), 0x27C, 50, 4, 1, 2560, 446);
  /* FIELD mode, 200 lines: doubled (the largest that fits 448), centred */
  check_display(disp(&mx_video_ntsc, 640, 200), 0x27C, 50 + 24, 4, 2, 2560, 400);
  /* VGA: one VCK a pixel at 1024 */
  check_display(disp(&mx_video_vga_1024, 1024, 768), 260, 30, 1, 1, 1024, 768);

  /* the circuit's own magnification and position; DX/DY do not go below 0 */
  struct mx_rt t;
  struct mx_display d;
  mx_rt_init(&t, 0, 320, 224, MX_GS_PSM_CT16S, 0, MX_RT_NOZ);
  mx_display_init(&d, &t);
  d.c[1].magh = 8;
  d.c[1].dx = -4;
  d.c[1].dy = -1000;
  d.c[1].dbx = 3;
  d.c[1].dby = 5;
  mx_display_regs(&mx_video_ntsc, &d, r);
  check_display(r[MX_DISP_DISPLAY2], 0x27C - 4, 0, 8, 2, 2560, 448);
  CHECK_EQ(r[MX_DISP_DISPFB2], MX_SET(GS_DISPFB2_FBW, 5) | MX_SET(GS_DISPFB2_PSM, MX_GS_PSM_CT16S) |
                                   MX_SET(GS_DISPFB2_DBX, 3) | MX_SET(GS_DISPFB2_DBY, 5));

  /* a fade to BGCOLOR: circuit 1 over BGCOLOR at a constant alpha */
  d.c[0] = d.c[1];
  d.c[1].on = 0;
  d.slbg = 1;
  d.alp = 0x40;
  d.bgcolor = 0x123456;
  mx_display_regs(&mx_video_ntsc, &d, r);
  CHECK_EQ(r[MX_DISP_PMODE], MX_SET(GS_PMODE_EN1, 1) | MX_SET(GS_PMODE_CRTMD, 1) |
                                 MX_SET(GS_PMODE_MMOD, 1) | MX_SET(GS_PMODE_AMOD, 1) |
                                 MX_SET(GS_PMODE_SLBG, 1) | MX_SET(GS_PMODE_ALP, 0x40));
  CHECK_EQ(r[MX_DISP_DISPLAY2], 0);
  CHECK_EQ(r[MX_DISP_BGCOLOR], 0x123456);
}

static void targets(void) {
  CHECK_EQ(mx_rt_pages(640, 448, MX_GS_PSM_CT32), 140);
  CHECK_EQ(mx_rt_pages(640, 448, MX_GS_PSM_Z24), 140);
  CHECK_EQ(mx_rt_pages(640, 448, MX_GS_PSM_CT16S), 70);
  CHECK_EQ(mx_rt_pages(640, 448, MX_GS_PSM_Z16), 70);
  CHECK_EQ(mx_rt_pages(1280, 720, MX_GS_PSM_CT32), 460);
  CHECK_EQ(mx_rt_pages(256, 256, MX_GS_PSM_T8), 8);
  CHECK_EQ(mx_rt_pages(256, 256, MX_GS_PSM_T4), 4);
  CHECK_EQ(mx_rt_pages(1, 1, MX_GS_PSM_CT32), 1);

  struct mx_rt t;
  mx_rt_init(&t, 10, 128, 100, MX_GS_PSM_CT32, 0, MX_RT_NOZ);
  CHECK_EQ(mx_rt_tex0(&t, 1, 0), mx_gs_tex0(320, 2, MX_GS_PSM_CT32, 7, 7, 1, 0));
  mx_rt_init(&t, 0, 640, 448, MX_GS_PSM_CT16S, 0, MX_RT_NOZ);
  CHECK_EQ(mx_rt_tex0(&t, 0, 1), mx_gs_tex0(0, 10, MX_GS_PSM_CT16S, 10, 9, 0, 1));
  mx_rt_init(&t, 0, 1280, 16, MX_GS_PSM_CT32, 0, MX_RT_NOZ);
  CHECK_EQ(mx_rt_tex0(&t, 0, 0), 0);

  /* the environment gs.c wrote for 640 x 224, in context 1 and 2 */
  static u64 a[16] __attribute__((aligned(16)));
  mx_rt_init(&t, 70, 640, 224, MX_GS_PSM_CT32, 140, MX_GS_PSM_Z24);
  for (int ctx = 0; ctx < 2; ctx++) {
    u64 *c = mx_rt_e_env(a, &t, ctx, 1);
    CHECK_EQ(c - a, 10);
    CHECK_EQ(a[0], mx_giftag(4, 1, -1, MX_GIF_FLG_PACKED, 1));
    CHECK_EQ(a[2], mx_gs_frame(70, 10, MX_GS_PSM_CT32, 0));
    CHECK_EQ(a[3], MX_GS_REG_FRAME_1 + ctx);
    CHECK_EQ(a[4], mx_gs_zbuf(140, MX_GS_PSM_Z24, 0));
    CHECK_EQ(a[5], MX_GS_REG_ZBUF_1 + ctx);
    CHECK_EQ(a[6], mx_gs_xyoffset((2048 - 320) << 4, (2048 - 112) << 4));
    CHECK_EQ(a[7], MX_GS_REG_XYOFFSET_1 + ctx);
    CHECK_EQ(a[8], mx_gs_scissor(0, 639, 0, 223));
    CHECK_EQ(a[9], MX_GS_REG_SCISSOR_1 + ctx);
  }
  CHECK_EQ(MX_GS_REG_FRAME_2, MX_GS_REG_FRAME_1 + 1);
  CHECK_EQ(MX_GS_REG_TEX0_2, MX_GS_REG_TEX0_1 + 1);
  CHECK_EQ(MX_GS_REG_MIPTBP2_2, MX_GS_REG_MIPTBP2_1 + 1);

  /* the clear: a sprite of the context over the whole target */
  u64 *c = mx_rt_e_clear(a, &t, 1, 1, 2, 3, 1);
  CHECK_EQ(c - a, 10);
  CHECK_EQ(MX_GET(GS_PRIM_CTXT, a[2]), 1);
  CHECK_EQ(a[6], mx_gs_xyz(mx_rt_px(&t, 0), mx_rt_py(&t, 0), 0));
  CHECK_EQ(a[8], mx_gs_xyz((2048 + 320) << 4, (2048 + 112) << 4, 0));

  /* no Z buffer: ZBUF masked */
  mx_rt_init(&t, 0, 64, 64, MX_GS_PSM_CT32, 0, MX_RT_NOZ);
  mx_rt_e_env(a, &t, 0, 1);
  CHECK_EQ(MX_GET(GS_ZBUF_ZMSK, a[4]), 1);
}

void test_video(void) {
  same_as_before(mx_video_ntsc, 224, 0x27C, 50);
  same_as_before(mx_video_ntsc, 448, 0x27C, 50);
  same_as_before(mx_video_pal, 256, 0x290, 72);
  same_as_before(mx_video_pal, 512, 0x290, 72);
  fitting();
  targets();
}
