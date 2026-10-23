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
