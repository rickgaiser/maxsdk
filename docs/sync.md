# Keeping all units busy

## FPU + VU0 + VU1 polynomial demo (`apps/llvm_bench/vudemo`, 2026-10-23, EE cycles per quadword, D = 16)

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

## The same demo on maxsdk (`apps/llvm_bench/vudemo`, 2026-10-23, PS2, no ps2sdk)

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

## EE + VU0 next to VU1: the PATH3 stream (2026-10-28, PCSX2 and the PS2)

The EE User's Manual's "macro parallel connection" (9.1.3) and SCEE's "A Tale of Two Dinosaurs"
(2000: EE + VU0 9.75, VU1 12.75, both 22.5 Mvert/s): VU1 draws over PATH1 from the VIF1 stream
while the EE with VU0 builds GS packets for PATH3. maxsdk: `mx_frame3_*` (`src/gs/frame3.c`,
chain mode).

- The PATH3 packets are built in their own scratchpad halves, copied into slots (fromSPR) and
  sent by a DMA queue on the GIF channel. Chain mode's fromSPR copies rule out a GIF MFIFO.
- They draw with GS context 2. The GIF takes turns at packet ends (EOP), so the streams'
  packets interleave in the GS, in no known order: Z-tested opaque geometry.
- Frames are kept apart without MSKPATH3: an open mark in the VIF1 stream (MARK, I-bit NOP)
  where the frame's first PATH3 packet was reserved; the VIF1 interrupt queues the slots held
  until then. A join mark before the FINISH (FLUSH, MARK, I-bit NOP, FLUSHA): released by the
  VIF1 or the GIF queue's interrupt once the queue has sent all of the frame's slots, so FLUSHA
  then sees exactly the frame's PATH3 data. A FLUSHA without the join can pass in the gap
  between two queued segments, and frame n + 1's slots (held until their open mark) cannot
  slip in before it.
- `mx_vu0r_scei` (`src/vu/vu0r.c`, `vu0scei.vsm`): mscei's math on VU0, two register sets, the
  results stored by SQC2 straight into PACKED GS packets (ST, RGBAQ, XYZ2) of the PATH3 stream.
  In the compiled mscei's order of operations (the MVP's w column first, the lights y x z, the
  colour as integers in the low bits of x + 1.5 · 2^23): the model demo's first copy is the
  same picture, bit for bit, by VU1 and by the EE and VU0 (`apps/model`, `-vu0`), on PCSX2
  and on the PS2.
- The PS2's first measurement (model bench, MSCEI strips, 8 x 8 copies, the first copy by EE +
  VU0): 208.6 fps, VU1 83 %, EE 99 %, against 241.8 fps, VU1 98 %, EE 31 % with VU1 alone. The
  EE is the bottleneck: about 1 M cycles a frame more for the hero's 832 vertices, far more
  than the renderer's work. Suspect: PATH3 only gets the GIF while PATH1/2 are idle, VU1 keeps
  PATH1 busy, the hero's ~41 KB of packets do not fit the 16 slots (32 KB), and the EE waits
  for slots (`mx_frame3_stats.wait_slot`). Not measured yet.
- Without skinning (Queen Marika, FullClothes, `-static`, one copy, MSCEI strips, the whole model
  the hero): VU1 39.4 fps, 13.4 M strip vertices/s (VU1 99 %, EE 19 %); EE + VU0 18.5 fps,
  6.2 M/s (EE 99 %): about 48 EE cycles a vertex (Dinosaurs: 9.75 M/s). Both at once on one
  model (VU0 about a third of the strips) would be about 19 M/s.

## Phases 5-7: what is built, and what the PS2 measured (2026-10-23)

Built, correct on PCSX2 and on the PS2 (batch ELF, 14 of 14 parts). PS2 numbers: see the
table after this one.

| Pattern | Where | To measure on the PS2 |
|---|---|---|
| Fences: DMA end interrupt vs polled VIF MARK (one chain, a MARK per batch) vs GS LABEL | `probe/p5_fast.c` | latency and per-batch cost of each; the default fence (D13) follows from it. `mx_vuq_wait` already polls the MARK (no interrupt on the wait path) |
| Render pipeline: stream built in two SPR halves, pushed into an MFIFO ring, VIF1 drains it all the time; VU1 render kernel double-buffered (BASE/OFFSET + xtop), XGKICK; FINISH per frame, vblank swap | `include/mx/frame.h`, `examples/scene/scene.c` | frames per vsync, missed / starved vblanks, EE wait for the back buffer and for ring space, busy shares; double-buffered vs FLUSHE after every batch (no vsync, 256 cubes) |
| Textures over PATH3 next to the stream; the stream waits with FLUSHA before the geometry that uses them | `examples/scene/scene.c`, `examples/allunits/allunits.c` | PATH3 vs PATH1 interleave in the busy shares |
| mx_sched: VU workers take batches from a pool when idle, the EE takes small chunks; every wait polls the workers | `include/mx/sched.h`, `apps/llvm_bench/vudemo` (MODE_SCHED) | cycles/qw vs the static schedule (31.00) and how the pool spreads over VU0, VU1 and the FPU |
| All units: VU1 render + GS, PATH3 texture, VU0 particle step on VIF0, EE/FPU matrices and sprites, step f-1 drawn in frame f | `examples/allunits/allunits.c` | busy share of every unit; every idle gap above 1 % of a frame needs a cause here |
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

## VU waits and code layout: findings from reverted prototypes (PS2, 2026-10-24)

The library changes behind these measurements were reverted. The probes, tests and the full
patch are kept next to maxsdk in `maxsdk_vu_work_2026-10-24/` (`probe/p12_vustate.c`,
`probe/p13_thold.c`, `tests/ps2/dbl1t.vsm`; the patch builds on b6e5657).

### Polling while DMA runs (`p12_vustate.c`)

A VIF1 DMA of 4 × 1000 quadwords (UNPACK V4-32) while the EE polls a register between CHCR
checks, median of 9:

| EE polls | Cycles | vs idle |
|---|---|---|
| nothing (64-cycle delays) | 19 671 | |
| VPU-STAT (cfc2) | 19 975 | +1.5 % |
| CHCR (uncached) | 21 873 | +11 % |
| VIF1_STAT | 22 183 | +13 % |
| uncached memory | 23 293 | +18 % |

Tight loops on uncached registers slow the DMA they wait for. Loops in maxsdk that do it:
`mx_vuq_wait` (MARK), the end of `wait_dbuf`, `vif_idle`, `mx_vuq_done` (on every
`mx_sched_poll`), `mx_dma_wait` (CHCR), `mx_mfifo_stop`.

Prototype: the VU waits poll VPU-STAT between MARK reads (until VBS changes while the VU is
busy: the MARK follows FLUSHE, so it cannot come earlier; 0 or 64 cycles while idle),
`mx_vuq_done` skips the MARK while the newest batch runs, `mx_dma_wait` waits 32 cycles between
CHCR reads. `probe/p4_vu.c`, mean over 5 code layouts (0–6 KB of padding before the probe's
code), median cycles/100 qw:

| p4_vu | before | idle gap 0 | idle gap 64 |
|---|---|---|---|
| vu1, batches of 30 | 7300 | +1.2 % | +0.6 % |
| vu1, batches of 60 | 5566 | +0.6 % | +0.3 % |
| vu1, batches of 240 / 960 | 4676 / 4466 | −0.7 / −0.8 % | −1.2 / −1.3 % |
| vu0, single buffer, 60 | 4014 | +1.0 % | +0.6 % |
| vu0, double buffer, 60 | 6798 | +1.0 % | +1.0 % |
| vu1, single buffer, 120 | 3051 | −0.6 % | −1.2 % |
| vu1, double buffer, 120 | 2904 | +0.9 % | +0.8 % |

Within ±1.3 % in p4_vu, where the EE mostly waits for the VU rather than for a long DMA;
`levels` and `p5_fast` did not change.

### Code layout decides the per-batch cost (`p4_vu.c`)

The EE I-cache is 16 KB, 2-way. The per-batch functions (VU queues, DMA queue, interrupt entry,
packet building) are spread over the program; when three of them map to the same sets they
evict each other on every batch. Padding alone moves the same build by up to 25 %: 1 KB in
`vu.c` took vu1 batches of 30 from 10580 to 8290 cycles/100 qw; padding before the probe's
code moves vu0 double buffer 6440–7156, vu1 double buffer 2771–3081, vu1 batches of 30
7146–7878. **A single build says little: compare over several paddings** (one build made the
polling prototype look like −6 % on the double buffer).

Prototype: the hot functions in one section at the start of `.text` (`section(".text.mx_hot")`,
below 8 KB, cold parts such as the MPG upload moved out). Medians, cycles/100 qw, one layout
each:

| p4_vu | HEAD b6e5657 | hot section | hot section, 1 KB padding in libmx / in the probe |
|---|---|---|---|
| vu1, batches of 30 | 8764 | 7618 | 7179 / 7704 |
| vu1, batches of 120 | 5250 | 4989 | 4977 / 5059 |
| vu0, single buffer, 60 | 4591 | 4108 | 4180 / 4130 |
| vu0, double buffer, 60 | 7315 | 6785 | 7338 / 7144 |
| vu1, single buffer, 120 | 3577 | 3077 | 3129 / 3100 |
| vu1, double buffer, 120 | 3639 | 2838 | 3036 / 3028 |

Measured from clean builds with one clang (the Makefile does not rebuild objects when the
compiler changes).

### Holding double-buffered batches by a T-bit stop (`p13_thold.c`)

`mx_vuq`'s double buffer holds the VIF with an i-bit before each MSCAL until the EE has read
the previous results (an INTC interrupt resumes the VIF). Alternative: the kernel ends with a T
bit (delay slot a NOP) followed by the E bit (`dbl1t.vsm`). With TE1 set and no INT_VU1 handler,
VU1 stops after each batch and the VIF holds the next MSCAL by itself; the EE polls VPU-STAT
for VTS1, reads the batch (toSPR DMA from VU1 memory) and resets VU1 only (two FBRST writes,
~50 cycles, TE1 written again), which lets the VIF go on. The VIF unpacks batch k+2 into batch
k's buffer only after MSCAL k+1, so after the read.

VU1, 960 items in batches of 120, median cycles/100 qw, 3 code layouts:

| | pad 0 | pad 2240 | pad 4160 |
|---|---|---|---|
| mx_vuq single buffer | 3151 | 2999 | 3006 |
| mx_vuq double buffer (i-bit) | 3323 | 2787 | 3205 |
| T hold, a queued segment per batch (as mx_vuq submits) | 2246 | 2177 | 2157 |
| T hold, all batches in one chain | 2065 | 2070 | 2064 |
| T hold inside mx_vuq (dbuf = 2 prototype) | 2400 (one layout) | | |

Per batch (one chain): the EE waits 1530 cycles for the stop, reads for 860, releases in 45.
All results correct; 20–33 % faster than the i-bit hold, independent of the layout. Found with
it:

* With TE1 **off**, the PS2 passes over the T bit: the T + E kernel ends at the E bit, correct
  results, VU1 Ready (and in the i-bit queue as fast as `dbl1db`). So one kernel serves both
  holds.
* PCSX2 shows no VTS1 for the T stop: a T hold needs a fallback there (ForceBreak from Ready
  setting VFS1 is a usable check; on the PS2 VFS1 is not set at once, poll up to ~1000 cycles).
* The `mx_vuq` prototype hung the PS2 in `tests/ps2/vu.c`; with prints and delays between the
  steps it passed. Suspected, not verified: VTS1 still reads set for a moment after the
  VU-only reset, and a wait straight after the release takes it for the next batch's stop.
