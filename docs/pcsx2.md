# Where PCSX2 differs from the PS2

Correctness notes only. PCSX2 (flatpak 2.5.x) timings are never performance data.

| Behaviour | PS2 | PCSX2 | Source |
|---|---|---|---|
| Multiplier with a power of two in fs | 1 ULP low | exact in both orders | `bench/vudemo/mulprobe.c` |
| DMA tag with an uncached (0x2…) address | crash | masked, works | `bench/vudemo` |
| Mapped VU memory writes + CMSAR1/VCALLMS start | wrong VU results | works | `bench/vudemo` |
| VIF0 chain with tags written uncached | does not start | runs | `bench/vudemo` |
| Debug output | SIF tty → ps2link → ps2client | SIF tty → IOP console, EE SIO → EE console (both go to stdout with `-earlyconsolelog`) | maxsdk `tests/ps2/hello.c` |
| After `Exit` | back to ps2link | loads `rom0:OSDSYS`, fails, reboots the IOP | maxsdk `tests/ps2/hello.c` |
| Stall control with the drain (VIF1) in **normal** mode | stalled (manual: normal-mode drains are stall controlled) | not stalled: the drain read the buffer before the source wrote it | maxsdk `tests/ps2/dma.c` (first version) |
| Stall control with a `refs` packet on a chain drain | stalls; data correct (`tests/ps2/dma.c`) | stalls until the source has written | maxsdk `tests/ps2/dma.c` |
| DMA transfers in the engine test | take time; the queue fills | finish almost at once (each segment done before the next is submitted) | maxsdk `tests/ps2/dma.c` |
| fromSPR reading a source that the EE rewrites right after `mx_mfifo_push` | 53 of 192 words corrupted | no corruption (the DMA is done at once) | maxsdk `tests/ps2/dma.c` (first version) |
| VIF0 chain with tags written uncached (isolated probe) | starts, data ok | starts, data ok | maxsdk `probe/p2_dma.c` |
| Reading VU memory while the VU runs | wrong data | correct data | maxsdk `tests/ps2/vu.c` |
| VU1 D bit with FBRST.DE1 | hangs the PS2 | ignored | maxsdk `tests/ps2/dbit.c` |
| spin demo, 120 frames | 146 vsyncs | 119 vsyncs | maxsdk `demos/spin.c` |
| A second SIGNAL before CSR.SIGNAL is cleared (GS interrupts masked) | the same: the GS stops until CSR.SIGNAL is written | the GS stops at the second SIGNAL (SIGID 0x11, the LABEL after it not yet set) and goes on once CSR.SIGNAL is written | maxsdk `probe/p5_fast.c` |
| Stall control, normal-mode VIF1 drain (`probe/p5_fast.c` stall_normal) | the drain waits; data correct | the drain does not wait: 28 of 28 words wrong | maxsdk `probe/p5_fast.c` |
| Timing of everything in phases 5-7 (frame pipeline, mx_sched, sampling) | | DMA, VU and GS work finish almost at once: busy shares read 0 %, there are no missed vblanks | maxsdk `demos/scene.c`, `demos/allunits.c` |
