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
