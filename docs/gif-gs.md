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
| spin demo (synchronous frame loop: PATH3 + PATH1 + FINISH per frame) | 120 frames in 146 vsyncs (PCSX2: 119) | | `demos/spin.c` | 2026-10-23 |

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
| spin on the frame pipeline: 120 frames in 119 vsyncs, no missed vblank (the phase 3 synchronous loop: 146 vsyncs) | | | `demos/spin.c` | 2026-10-23 |
| scene, 64 textured lit cubes + a 64x64 texture streamed every frame: 240 frames in 239 vsyncs, missed 0; EE builds a frame in 70 k cycles | | | `demos/scene.c` | 2026-10-23 |
| scene without vsync, 256 cubes (3072 triangles): VU1 batches double buffered **440 k cycles/frame**, a FLUSHE after every batch 581 k: double buffering saves 24 % on the render path | | | `demos/scene.c` | 2026-10-23 |

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
| SMODE2.FFMD = 1 (FRAME: every line in each field) with a 224-line buffer | both fields show the same image, half a line apart: the picture bobs at 60 Hz (seen as flicker of every edge on a TV) | interlaced NTSC | user on the TV, `demos/procgen.c` | 2026-10-23 |
| 640×448 at 60 fields/s: FFMD = 0 (FIELD: every other line per field) from a 448-line buffer, DISPLAY.DH = 447 | `mx_gs_init(640, 448)`; each frame is drawn in full; two buffers + Z24 take 3.4 of the 4 MiB | | `demos/procgen.c` | 2026-10-23 |
| No vsync, two buffers: the display switched at the vblank only | the next frame was already drawn into the buffer on screen (the whole screen flickers); now a frame is shown at its FINISH (it tears), before the GS starts on the next | `mx_frame_vsync(0)` | user on the TV | 2026-10-23 |
| The first frame of a run is submitted at any point of the field | its first vblank is not a missed one: `mx_gs.missed` counts from the vblank after the run's first frame is shown | | `demos/procgen.c` | 2026-10-23 |
| `mx_gs.slack`: EE cycles from a frame's FINISH to the vblank that shows it | procgen keeps 8–20 % of a field (its view distance follows it) | vsync on | `demos/procgen.c` | 2026-10-23 |

Consequences: the GS draws at most one Gouraud/textured triangle per 8 EE cycles (≈ 615 k
per 60 Hz frame, 37 M/s). Strips cost one vertex per triangle, so PATH1 sending (2 cycles
per register) stays under the GS floor up to 4 registers per vertex: PACKED is enough, REGLIST
gains nothing on PATH1. Lists send 3 vertices per triangle and become transfer-bound (12–18
cycles per triangle on PATH1).
