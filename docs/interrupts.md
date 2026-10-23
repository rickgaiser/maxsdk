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
| After a T-bit stop VPU-STAT.VTS1 (0x400) stays set; maxsdk resets VU1 before the next MSCAL | | PS2 and PCSX2 | `tests/ps2/vu.c` |
| **VU1 D bit with FBRST.DE1 set hangs the PS2**, also when the VU1 interrupt handler resets VU1 (two runs). PCSX2 ignores the D bit. maxsdk does not use it. | | | `tests/ps2/dbit.c` (first two versions) |
| With FBRST.DE1 clear, VU1 runs past a D bit to its E bit, without an interrupt | | | `tests/ps2/dbit.c` |
| VIF i-bit stop + resume (FBRST.STC) per batch, used by the double-buffered VU queues | works on every batch | | `tests/ps2/vu.c` |
| Resetting VU1 through FBRST also clears FBRST.TE1/DE1 (the register is written whole) | | | `probe/p4_vu.c` |
| Timer 1 compare interrupt at 1 kHz (BUSCLK/256, compare 576): 49 interrupts in 50 ms | | | `tests/ps2/dma.c` |

Known from the manual (EE Kernel Reference, AddIntcHandler): handlers run with interrupts disabled,
with the gp of the code that registered them; GPRs are saved, FPU registers are not; returning -1
skips the later handlers of the same cause.

Consequence: an interrupt-refilled DMA queue (D11b) restarts a channel about 300 cycles after a
segment ends (≈195 dispatch + ≈100 in the handler, see dma.md). Segments should carry at least a
few hundred cycles of work; polling is cheaper for very short ones.
