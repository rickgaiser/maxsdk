# GIF and GS

| Fact | Value | Conditions | Source | Date |
|---|---|---|---|---|
| FRAME/ZBUF/DISPFB count 2048-word pages, TEX0/BITBLTBUF 64-word blocks (`MX_GS_PAGE`) | | manual; a readback with a page number as block number read the wrong buffer | `tests/ps2/gs.c` | 2026-10-23 |
| DISPFB/DISPLAY/PMODE are write-only (reads give garbage); maxsdk keeps a shadow | | | `tests/ps2/gs.c` | 2026-10-23 |
| PATH3 (GIF channel) IMAGE upload, 64 KiB | 4.49 / 4.52 cycles/qw | CT32 | `probe/p3_gs.c` | 2026-10-23 |
| PATH2 (VIF1 DIRECT) IMAGE upload, 64 KiB | 5.46 / 5.49 cycles/qw | | `probe/p3_gs.c` | 2026-10-23 |
| PATH1 (VU1 XGKICK) IMAGE of 960 qw from VU1 memory, incl. the VIF1 start | median 3.62 cycles/qw (min was an outlier) | | `probe/p3_gs.c` | 2026-10-23 |
| PATH3 and PATH2, 64 KiB each, started together | 43369 cycles with GIF_MODE.IMT = 0, 41793 with IMT = 1 (≈ the sum: the paths take turns) | | `probe/p3_gs.c` | 2026-10-23 |
| Clear 640×224 32-bit colour + 24-bit Z with one sprite | 42798 / 43198 cycles | | `probe/p3_gs.c` | 2026-10-23 |
| vsync period | 4 920 116 cycles (59.94 Hz) | NTSC interlaced frame mode | `probe/p3_gs.c` | 2026-10-23 |
| 100 A+D writes with the checked packet builder | into uncached 2637, into SPR 2220 cycles (22–26 per quadword) | | `probe/p3_gs.c` | 2026-10-23 |
| SIGNAL with a SIGLBLID update, LABEL, FINISH, IMR via `GsPutIMR`, local→host readback (VIF1 FDR + BUSDIR) | work as the manual says | | `tests/ps2/gs.c` | 2026-10-23 |
| spin demo (synchronous frame loop: PATH3 + PATH1 + FINISH per frame) | 120 frames in 146 vsyncs (PCSX2: 119) | | `demos/spin/spin.c` | 2026-10-23 |

Consequence: the checked packet builder costs ~25 cycles per quadword, 5–6x the DMA cost of the
same data; hot loops should use the cursor emitters (D5a) or constant packets.

## Phases 5-6 (PS2, 2026-10-23)

| Fact | Value | Conditions | Source | Date |
|---|---|---|---|---|
| Cursor emitters (`<mx/emit.h>`, two `sd` per quadword), 100 A+D into SPR | 2.63 cycles/qw (checked builder 23.1) | | `probe/p5_fast.c` | 2026-10-23 |
| The same with `pcpyld` + `sq` (inline asm) | 5.08 cycles/qw: slower than two `sd` | | `probe/p5_fast.c` | 2026-10-23 |
| Emitters, 150 PACKED RGBAQ + XYZ2 vertices into SPR | 3.16 cycles/qw (checked builder 34.8) | | `probe/p5_fast.c` | 2026-10-23 |
| GS LABEL, GIF DMA start until the EE sees it (polled SIGLBLID) | 236 / 323 cycles | 2-qw A+D packet | `probe/p5_fast.c` | 2026-10-23 |
| A second SIGNAL while CSR.SIGNAL is still set **stops the GS** (the LABEL after it is not written); writing CSR.SIGNAL lets it go on, 208 cycles to the LABEL | | GS interrupts masked | `probe/p5_fast.c` | 2026-10-23 |
| spin on the frame pipeline: 120 frames in 119 vsyncs, no missed vblank (the phase 3 synchronous loop: 146 vsyncs) | | | `demos/spin/spin.c` | 2026-10-23 |
| scene, 64 textured lit cubes + a 64x64 texture streamed every frame: 240 frames in 239 vsyncs, missed 0; EE builds a frame in 70 k cycles | | | `demos/scene/scene.c` | 2026-10-23 |
| scene without vsync, 256 cubes (3072 triangles): VU1 batches double buffered **440 k cycles/frame**, a FLUSHE after every batch 581 k: double buffering saves 24 % on the render path | | | `demos/scene/scene.c` | 2026-10-23 |

## Triangles (PLAN_PROCGEN P0, PS2, 2026-10-23, `probe/p9_gs.c`)

640×224, 32-bit colour, Z24 test GEQUAL with Z writes. EE cycles. PATH3 = 4096 triangles in
one GIF DMA from memory; PATH1 = 256 triangles kicked 9 times from VU1 memory (difference to
one kick). "side s" = right triangles of s×s/2 pixels in rows; ADC = vertices sent, not drawn.

| Fact | Value | Conditions |
|---|---|---|
| **GS triangle setup floor, Gouraud or textured** | **8.0 cycles per triangle** (4 GS cycles) | strips of 2–8 px, PATH1 PACKED or REGLIST, PATH3 REGLIST |
| … flat (IIP = 0, untextured) | 4.2 cycles per triangle | PATH3 REGLIST strips, 2 px |
| **PATH1 transfer (ADC)** | **2.0 cycles per GS register written**: PACKED 2.0 cycles/qw, REGLIST 4.0 cycles/qw | REGLIST halves the qw, not the time |
| PATH3 from memory (ADC) | 4.6–4.8 cycles/qw PACKED, 4.7 REGLIST (9.5 per qw-pair of regs) | the GIF DMA from memory is the limit |
| Fill, large triangles (512 px) | flat / Gouraud 0.164, textured point 0.314, textured bilinear 0.42 cycles per pixel | 64×64 CT32 texture, modulate |
| Size where fill passes the setup floor | ~50 px flat/Gouraud, ~25 px textured point, ~20 px bilinear | |
| PATH1 strip vertex, Gouraud: 2 regs = 4 cycles to send, 8 to set up | GS-bound at 8 | |
| PATH1 strip vertex, textured: 3 regs = 6 cycles to send, 8 to set up | GS-bound at 8 (12.4 at 32 px, 45 at 128 px) | |
| Lists over PATH3 | 14 / 28 / 42 cycles per triangle (flat / Gouraud / textured PACKED): transfer-bound | 3 vertices per triangle |
| **The GIF tag's PRIM field is ignored in REGLIST mode** | the previous PRIM stays (a SPRITE after a clear drew sprites); write PRIM by A+D first | PS2 and PCSX2 |

## Display (PLAN_PROCGEN P4, PS2, 2026-10-23)

| Fact | Value | Conditions | Source | Date |
|---|---|---|---|---|
| SMODE2.FFMD = 1 (FRAME: every line in each field) with a 224-line buffer | both fields show the same image, half a line apart: the picture bobs at 60 Hz (seen as flicker of every edge on a TV) | interlaced NTSC | user on the TV, `demos/procgen/procgen.c` | 2026-10-23 |
| 640×448 at 60 fields/s: FFMD = 0 (FIELD: every other line per field) from a 448-line buffer, DISPLAY.DH = 447 | `mx_gs_init(640, 448)`; each frame is drawn in full; two buffers + Z24 take 3.4 of the 4 MiB | | `demos/procgen/procgen.c` | 2026-10-23 |
| No vsync, two buffers: the display switched at the vblank only | the next frame was already drawn into the buffer on screen (the whole screen flickers); now a frame is shown at its FINISH (it tears), before the GS starts on the next | `mx_frame_vsync(0)` | user on the TV | 2026-10-23 |
| The first frame of a run is submitted at any point of the field | its first vblank is not a missed one: `mx_gs.missed` counts from the vblank after the run's first frame is shown | | `demos/procgen/procgen.c` | 2026-10-23 |
| `mx_gs.slack`: EE cycles from a frame's FINISH to the vblank that shows it | procgen keeps 8–20 % of a field (its view distance follows it) | vsync on | `demos/procgen/procgen.c` | 2026-10-23 |

## Render targets, both contexts, display states (PS2, 2026-10-27, `tests/ps2/rt.c`)

| Fact | Value | Conditions | Source | Date |
|---|---|---|---|---|
| Context 2 draws into a second target while context 1 keeps the frame buffer; switching per primitive by PRIM.CTXT alone (no FRAME rewrite, no FLUSH) | correct pixels in both | 128×128 CT32 target, direct sprites (PATH2) | `tests/ps2/rt.c` | 2026-10-27 |
| A target is read as a texture right after it is drawn, with only a TEXFLUSH (after a VIF FLUSH) between | correct, CT32 and CT16S targets (16-bit expanded as `v << 3`) | TEX0 from `mx_rt_tex0`, decal, nearest | `tests/ps2/rt.c` | 2026-10-27 |
| Moving context 1 to the target and back (`mx_frame_target`: VIF FLUSH + FRAME/ZBUF/XYOFFSET/SCISSOR) inside a frame | correct, in ring and chain mode | | `tests/ps2/rt.c` | 2026-10-27 |
| Three display states (`mx_gs_displays(d, 3)`): frame k in buffer k % 3, waits for frame k − 2 to be shown | the three buffers hold the last three frames; 0 vblanks missed | 640×224 CT32 | `tests/ps2/rt.c` | 2026-10-27 |
| `mx_gs_init` writing DISPFB1/DISPLAY1 = 0 (circuit 1 off) and, after SetGsCrt, all display registers = 0 before the real display | **the PS2 hung or dropped ps2link before the first printf** (twice; PCSX2 fine; once a run passed). Fixed by never writing an off circuit's registers and writing PMODE, DISPFB2, DISPLAY2, BGCOLOR before the GS interrupts are on, as the old init did; which of the two mattered is not isolated | NTSC 640×224 | `tests/ps2/gs.c`, batch | 2026-10-27 |
| After `mx_video_set` (GS reset) PRMODECONT, COLCLAMP, TEST, TEXA must be written again before drawing | | | `tests/ps2/rt.c`, `demos/display` | 2026-10-27 |

## Video modes (`src/gs/video.c`)

DISPLAY units: DX/DW in VCK, DY/DH in raster lines (frame lines when interlaced). A buffer line
takes 2 DISPLAY lines in interlaced FRAME mode, else 1; `mx_display_regs` fits the largest
MAGH/MAGV and centres the rectangle (by whole frames when interlaced).

"Runs": the CRTC gives vblanks and `demos/display` draws in the mode (PS2, BIOS 2.00 PAL slim,
2026-10-27). "Screen": the picture's position was checked on a screen.

| Mode | SetGsCrt | VCK × lines | DX, DY | Runs | Screen |
|---|---|---|---|---|---|
| NTSC 480i / 240p | 0x02 | 2560 × 448 / 224 | 0x27C, 50 / 25 | yes | 480i yes (sceGsSetDefDispEnv's); 240p no |
| PAL 576i / 288p | 0x03 | 2560 × 512 / 256 | 0x290, 72 / 36 | yes | no |
| 480p | 0x50 | 1440 × 480 | 232, 35 | yes | no (gsKit's) |
| 576p | 0x53 | 1440 × 576 | 255, 44 | **no: SetGsCrt returns, no vblank ever comes** | |
| 720p | 0x52 | 1280 × 720 | 306, 24 | yes | no (gsKit's) |
| 1080i | 0x51 | 1920 × 1080 | 236, 38 | yes (FRAME mode) | no (gsKit's) |
| VESA 640/800/1024/1280 | 0x1A, 0x2B, 0x3D, 0x4A | 1280×480, 1600×600, 1024×768, 1280×1024 | gsKit's | yes | no |

A mode without vblanks hung every frame pipeline (each frame waits for the previous one to be
shown at a vblank): `mx_video_set` now waits up to 100 ms for two vblanks and returns -1.

`demos/display` shows every mode with a border at the picture's edge and lets the offsets be
moved with the pad (the values are on screen); offsets confirmed there go into this table.

Consequences: the GS draws at most one Gouraud/textured triangle per 8 EE cycles (≈ 615 k
per 60 Hz frame, 37 M/s). Strips cost one vertex per triangle, so PATH1 sending (2 cycles
per register) stays under the GS floor up to 4 registers per vertex: PACKED is enough, REGLIST
gains nothing on PATH1. Lists send 3 vertices per triangle and become transfer-bound (12–18
cycles per triangle on PATH1).

## The GS at its manual rates: gsmax (PS2, 2026-10-25)

`demos/gsmax/gsmax.c`: every row of GS User's Manual 1.3 "Maximum Performance", PATH1 (a
packet in VU1 memory kicked 9 times by `probe/kickn.vsm`, (9 kicks − 1 kick) / 8), REGLIST,
primitives of 1–2 pixels, Z test GEQUAL with writes. GS cycles = EE cycles / 2. Floor = the
larger of the manual's setup cycles and the registers sent (one per GS cycle).

| Case | Regs/prim | Manual | Measured (GS cycles) | M prims/s | Of floor |
|---|---|---|---|---|---|
| tri flat | 1 | 2 | 2.25 | 65.4 | 88 % |
| tri flat fog | 1 | 2 | 2.34 | 62.9 | 85 % |
| tri Gouraud / tex flat / tex Gouraud | 2 / 2 / 3 | 4 | 4.00 | 36.9 | 100 % |
| tri Gouraud fog / tex fog / tex Gouraud fog | 2 / 2 / 3 | 5 | 5.00 | 29.5 | 99 % |
| tri flat AA1 | 1 | 6 | 6.43 | 22.9 | 93 % |
| tri Gouraud AA1 / tex AA1 | 2 | 8 | 8.00 | 18.4 | 100 % |
| tri Gouraud fog AA1 / tex fog AA1 | 2 | 9 | 9.00 | 16.4 | 100 % |
| line tex / tex Gouraud | 2 / 3 | 4 | 4.00 | 36.9 | 100 % |
| line tex fog | 2 | 5 | 4.99 | 29.5 | 100 % |
| line flat AA1 | 1 | 6 | 6.17 | 23.9 | 97 % |
| line tex AA1 | 2 | 7 | **6.00** | 24.6 | faster than the manual |
| sprite flat (2 × XYZ2) | 2 | 3 | 3.00 | 49.2 | 100 % |
| sprite tex (2 × UV + XYZ2) | 4 | 3 | **6.00** | 24.6 | 66 % of the 4-register floor |
| point flat (XYZ2) | 1 | 1 | 1.00 | 146.2 | 99 % |
| point Gouraud / tex | 2 | 1 | 2.00 | 73.4 | transfer-bound |

| Fill (Z + alpha blending on unless noted) | Measured px per GS cycle | Gpixel/s | Of 16 / 8 |
|---|---|---|---|
| untextured, one 640 × 224 sprite | 6.79 | 1.00 | 42 % |
| untextured, sprites of one page (64 × 32) | 13.59 | 2.00 | 84 % |
| untextured, columns 32 px wide | **15.29** | **2.25** | 95 % |
| untextured, pages, no Z / alpha | 13.55 | 1.99 | 84 % |
| textured (64 × 64 CT32, point), one sprite | 6.51 | 0.96 | 81 % |
| textured, pages / columns 32 / pages no Z, alpha | 7.59 / **7.63** / 7.60 | 1.12 | 95 % |

Consequences: the setup rates of the manual hold on PATH1 when every primitive sends no more
registers than it has setup cycles (points: XYZ2 only; Gouraud/textured triangles: up to 4).
Flat triangles fall short of 2 cycles (2.25). Textured sprites cost 6 GS cycles, not 3: draw
large textured areas as columns, not as many small sprites. For fill, draw in 32-pixel-wide
columns (graphtip: scan along the long side of a page): 95 % of the manual's pixel rate, one
big sprite reaches 42 %. Z and alpha cost nothing at these rates.
