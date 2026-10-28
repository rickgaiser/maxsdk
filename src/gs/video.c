#include <mx/video.h>

// clang-format off
/*                                              code int ffmd hz  vck_w lines   dx   dy */
const struct mx_video_mode mx_video_ntsc      = {0x02, 1, 0, 60, 2560, 448, 0x27C, 50};
const struct mx_video_mode mx_video_pal       = {0x03, 1, 0, 50, 2560, 512, 0x290, 72};
const struct mx_video_mode mx_video_ntsc_240p = {0x02, 0, 0, 60, 2560, 224, 0x27C, 25};
const struct mx_video_mode mx_video_pal_288p  = {0x03, 0, 0, 50, 2560, 256, 0x290, 36};
/* gsKit's (gsKit_set_buffer_attributes), not checked on a screen yet */
const struct mx_video_mode mx_video_480p      = {0x50, 0, 0, 60, 1440, 480, 232, 35};
const struct mx_video_mode mx_video_576p      = {0x53, 0, 0, 50, 1440, 576, 255, 44};
const struct mx_video_mode mx_video_720p      = {0x52, 0, 0, 60, 1280, 720, 306, 24};
const struct mx_video_mode mx_video_1080i     = {0x51, 1, 0, 60, 1920, 1080, 236, 38};
const struct mx_video_mode mx_video_vga_640   = {0x1A, 0, 0, 60, 1280, 480, 280, 18};
const struct mx_video_mode mx_video_vga_800   = {0x2B, 0, 0, 60, 1600, 600, 465, 25};
const struct mx_video_mode mx_video_vga_1024  = {0x3D, 0, 0, 75, 1024, 768, 260, 30};
const struct mx_video_mode mx_video_vga_1280  = {0x4A, 0, 0, 60, 1280, 1024, 350, 40};
// clang-format on

struct mx_video_mode mx_video;
