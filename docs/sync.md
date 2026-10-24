# Keeping all units busy

## FPU + VU0 + VU1 polynomial demo (`bench/vudemo`, 2026-10-23, EE cycles per quadword, D = 16)

| Configuration | Cycles |
|---|---|
| EE FPU | 122.1 |
| VU0 macro | 37.9 |
| VU0 micro incl. COP2 transfers | 50.2 |
| VU1 micro incl. DMA and readback | 61.8 |
| FPU + VU0 micro + VU1 micro together | **36.2** |

* Compute peak (kernels only, transfers free): 11.7 cycles/qword.
* The combined run is **EE-bound**: the EE is busy 81k of 92k cycles (per qword VU0 17 write + 8.5
  read, VU1 26 readback + ~2400 per batch to send, FPU 122). Bound for that design: ~25–28.
* What would lower it: VIF0 DMA for VU0 input, double-buffered VU1 batches, reductions instead of
  reading back every quadword.

## The same demo on maxsdk (`bench/vudemo`, 2026-10-23, PS2, no ps2sdk)

| Configuration | ps2sdk version | maxsdk, VU0 by COP2 | + VU0 by VIF0 DMA | + VU1 read back by DMA (default) |
|---|---|---|---|---|
| EE FPU | 122.1 | 122.07 | 122.09 | 122.10 |
| VU1 micro | 61.8 | 59.23 | 59.25 | **38.96** |
| VU0 macro | 37.9 | 37.74 | 37.76 | 37.74 |
| VU0 micro | 50.2 | 49.66 | 45.79 | 45.95 |
| FPU + VU0 micro + VU1 micro | 36.2 | 35.56 | 31.63 | **30.39** (31.00 with MARK-based completion) |

All within 4 ULP of the exact results. With VIF0 DMA for VU0 the EE no longer copies VU0's input
(send 6.4k instead of 22.3k cycles per run); with DMA readback VU1's results cost 5.5 instead of
26 cycles per quadword. The design bound estimated for the ps2sdk version (~25–28) assumed both
of these EE costs, so it is lower now; the remaining waits are the next target.

## DMA engine gaps (phase 2)

* Interrupt-refilled queue: +529 cycles per segment over one long transfer (≈195 interrupt
  dispatch, ≈100 in the handler, the rest restart and VIF pipeline).
* The demo is still synchronous per batch (wait, then mapped readback); toSPR readback of VU1
  (3.1 vs 26 cycles/qw) and double buffering are the next steps (phase 5).

## Phases 5-7: what is built, and what the PS2 measured (2026-10-23)

Built, correct on PCSX2 and on the PS2 (batch ELF, 14 of 14 parts). PS2 numbers: see the
table after this one.

| Pattern | Where | To measure on the PS2 |
|---|---|---|
| Fences: DMA end interrupt vs polled VIF MARK (one chain, a MARK per batch) vs GS LABEL | `probe/p5_fast.c` | latency and per-batch cost of each; the default fence (D13) follows from it. `mx_vuq_wait` already polls the MARK (no interrupt on the wait path) |
| Render pipeline: stream built in two SPR halves, pushed into an MFIFO ring, VIF1 drains it all the time; VU1 render kernel double-buffered (BASE/OFFSET + xtop), XGKICK; FINISH per frame, vblank swap | `include/mx/frame.h`, `demos/scene/scene.c` | frames per vsync, missed / starved vblanks, EE wait for the back buffer and for ring space, busy shares; double-buffered vs FLUSHE after every batch (no vsync, 256 cubes) |
| Textures over PATH3 next to the stream; the stream waits with FLUSHA before the geometry that uses them | `demos/scene/scene.c`, `demos/allunits/allunits.c` | PATH3 vs PATH1 interleave in the busy shares |
| mx_sched: VU workers take batches from a pool when idle, the EE takes small chunks; every wait polls the workers | `include/mx/sched.h`, `bench/vudemo` (MODE_SCHED) | cycles/qw vs the static schedule (31.00) and how the pool spreads over VU0, VU1 and the FPU |
| All units: VU1 render + GS, PATH3 texture, VU0 particle step on VIF0, EE/FPU matrices and sprites, step f-1 drawn in frame f | `demos/allunits/allunits.c` | busy share of every unit; every idle gap above 1 % of a frame needs a cause here |
| VU1 compute between render passes (D16 b): FLUSH, MPG, run, stop at a VIF i-bit, EE reads by toSPR, resume | `mx_frame_vu1_compute`, `tests/ps2/frame.c` | the GS idle time it costs per pass |

Found on the way:
* VU0 macro mode (COP2 arithmetic) and a running VU0 micro program use the same unit, so the
  "EE + VU0 macro builds packets while VU0 micro computes" line of the plan cannot happen at the
  same time; the all-units demo leaves macro mode out.
* Per-mesh constants (matrix, light) go with every VU1 batch (7 quadwords) instead of a resident
  header: a header change needs a FLUSHE, which ends the overlap of the batches before and after.

### PS2 results (2026-10-23)

| Run | Result |
|---|---|
| Fences | polled MARK 100 cycles vs DMA end interrupt 763; a MARK per batch in one chain 121 cycles/batch vs a queue segment 561 (`docs/dma.md`) |
| scene (64 cubes, vsync) | 1 frame per vsync, missed 0. Busy shares: VIF1 4 %, VU1 4 %, GIF path 4 %, GS FIFO 4 %. The rest of every frame the units wait for the vblank: the EE builds a frame in 70 k of 4.92 M cycles, then waits 4.81 M for the back buffer (2 framebuffers, D14) |
| scene without vsync (256 cubes) | 440 k cycles/frame double buffered, 581 k with a FLUSHE per batch |
| allunits | 1 frame per vsync, missed 0, 230 400 VU0 particle updates; VU0 step = FPU. Busy: VIF1 5 %, VU1 4 %, GIF path 5 %, GS FIFO 4 %, VU0 0 % (8 batches of 120 particles take microseconds per frame). The idle time is the vblank wait (4.70 M of 4.92 M cycles per frame); the workload is far below one frame, no gap comes from a unit waiting for another |
| vudemo via mx_sched | 27.32 cycles/qw vs 30.88 for the static schedule |
| Sampling | 632-667 cycles per sample (2 kHz: 0.4 % of the EE) |
