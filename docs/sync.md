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
