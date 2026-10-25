# Interrupts

Measured on the PS2 (2026-10-23) with BIOS dispatch (AddDmacHandler/AddIntcHandler through maxsdk's
dispatcher, `src/irq/irq.c`). Cycles are EE cycles (294.912 MHz), min / median of 8–16 runs.

| Fact | Value | Conditions | Source |
|---|---|---|---|
| DMA start to the first instruction of our DMAC handler | 276 / 276 | toSPR, 1 qw | `probe/p2_dma.c` |
| The same transfer, polled to CHCR.STR = 0 | 81 / 81 | | `probe/p2_dma.c` |
| → DMAC interrupt dispatch (end of transfer to handler) | **≈ 195** | difference of the two | `probe/p2_dma.c` |
| GS FINISH: GIF DMA start to the GS handler | 586 / 638 | 2-qw A+D packet | `probe/p3_gs.c` |
| GS SIGNAL: GIF DMA start to the GS handler | 498 / 544 | | `probe/p3_gs.c` |
| VU1 T-bit stop: VIF1 DMA start (MSCAL) to the VU1 handler | 422 / 430 | FBRST.TE1 = 1 | `probe/p4_vu.c` |
| VU1 E-bit stop with MSCAL + FLUSHE, polled until VIF1 idle and VU1 stopped | 216 / 230 | | `probe/p4_vu.c` |
| VIF1 i-bit: the VIF stops after the code, STAT.VIS and INT set (STAT 0x00000cc2), the VIF1 interrupt fires, FBRST.STC resumes | works | | `tests/ps2/vif.c` |
| After a T-bit stop VPU-STAT.VTS1 (0x400) stays set until a reset, or a resume that ends at an E bit; then the next MSCAL is taken (VU states: [vu1.md](vu1.md)). `tests/ps2/vu.c` resets VU1 | | PS2 | `tests/ps2/vu.c`, vustate.c (2026-10-24, in `maxsdk_vu_work_2026-10-24/`) |
| **A D bit with FBRST.DEn set hangs the PS2**, on VU1 (also when the VU1 interrupt handler resets VU1, two runs) and on VU0 (VCALLMS start, a handler that masks itself after 100 calls: not an interrupt storm). PCSX2 ignores the D bit. maxsdk does not use it. | | | `tests/ps2/dbit.c` (first two versions), vudbit.c (2026-10-24, in `maxsdk_vu_work_2026-10-24/`) |
| INT_VU0WD (VU0 watchdog): no interrupt within 5 s of a spinning VU0 | | | vustate.c (2026-10-24, in `maxsdk_vu_work_2026-10-24/`) |
| An interrupt cause raised while no handler is installed (a T bit with TEn set, a VIF i-bit stop that is polled) stays pending in I_STAT; the next handler installed for it fires at once. A double-buffered `mx_vuq` started after `mx_frame_vu1_compute` took the pending VIF1 i-bit for one of its own stops, and read every batch from the wrong buffer (3325 of 3840 values wrong; in ps2batch, `p4_vu`'s VU1 double buffer after `frame`). **Not fixed in maxsdk**: `mx_vuq` should clear the VIF's I_STAT bit (`MX_R32(MX_I_STAT) = 1u << cause`) before it installs its handler | | PS2 and PCSX2 | vustop.c, vu.c stale_ibit (2026-10-24, in `maxsdk_vu_work_2026-10-24/`) |
| With FBRST.DE1 clear, VU1 runs past a D bit to its E bit, without an interrupt | | | `tests/ps2/dbit.c` |
| VIF i-bit stop + resume (FBRST.STC) per batch, used by the double-buffered VU queues | works on every batch | | `tests/ps2/vu.c` |
| Resetting VU1 through FBRST also clears FBRST.TE1/DE1 (the register is written whole), and `mx_vu_reset(1)` / `(2)` also clears the other VU's TE/DE. A VU reset clears that VU's own TE/DE even when the same write sets them; the other VU's are kept if written back (not done by maxsdk) | | | `probe/p4_vu.c`, vustate.c (2026-10-24, in `maxsdk_vu_work_2026-10-24/`) |
| Timer 1 compare interrupt at 1 kHz (BUSCLK/256, compare 576): 49 interrupts in 50 ms | | | `tests/ps2/dma.c` |

Known from the manual (EE Kernel Reference, AddIntcHandler): handlers run with interrupts disabled,
with the gp of the code that registered them; GPRs are saved, FPU registers are not; returning -1
skips the later handlers of the same cause.

Consequence: an interrupt-refilled DMA queue (D11b) restarts a channel about 300 cycles after a
segment ends (≈195 dispatch + ≈100 in the handler, see dma.md). Segments should carry at least a
few hundred cycles of work; polling is cheaper for very short ones.

## Phases 5 and 8 (PS2, 2026-10-23)

| Fact | Value | Source | Date |
|---|---|---|---|
| Timer 1 sampling interrupt (`mx_trace_sample`), cost per sample | 632 / 667 cycles (4 / 16 kHz) | `probe/p5_fast.c` | 2026-10-23 |
| Semaphore round trip between two BIOS threads of the same priority (SignalSema + WaitSema each way) | 1831 cycles | `tests/ps2/thread.c` | 2026-10-23 |
| A DMA queue's end interrupt wakes a thread sleeping in `mx_event_sleep` (iSignalSema): end interrupt to that thread running | 973 cycles | `tests/ps2/thread.c` | 2026-10-23 |
