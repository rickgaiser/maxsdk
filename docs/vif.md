# VIF

| Fact | Value | Source | Date |
|---|---|---|---|
| `FLUSH` waits for the GIF too and stalls VIF1 (VGW); `FLUSHE` only waits for the micro program to end | | `bench/vudemo` | 2026-10-23 |
| UNPACK to VU1, 1024 vectors, VIF1 normal-mode DMA from memory, per vector: V4-32 | 4.76 / 4.82 cycles | `probe/p4_vu.c` | 2026-10-23 |
| … V3-32 | 3.55 / 3.61 cycles | `probe/p4_vu.c` | 2026-10-23 |
| … V2-16, V4-8, V4-5, S-32 | 2.10–2.12 cycles (the same for all four) | `probe/p4_vu.c` | 2026-10-23 |
| MPG upload of 1024 instructions, including building the packet with the checked builder | 24.9 / 25.7 cycles per instruction (the builder dominates) | `probe/p4_vu.c` | 2026-10-23 |
| All UNPACK formats, STMASK + STROW (ROW fill), STMOD offset mode, BASE/OFFSET + FLG double buffering (TOPS/TOP swap at MSCAL), ITOP, MARK, the i-bit, DIRECT: work as the manual says | | `tests/ps2/vif.c` | 2026-10-23 |

Consequence: compressed UNPACK formats cost less than half of V4-32 per vector on the bus side
(V2-16 2.1 vs 4.8 cycles), so UV/colour streams should use them.

## PLAN_PROCGEN (PS2, 2026-10-23)

| Fact | Value | Source | Date |
|---|---|---|---|
| **VIF1 stops with STAT.ER0 (DMAtag mismatch) on correct packets**: a V2-16 UNPACK of 72 vectors (18 qw, a `ref` tag with TTE in the MFIFO stream) left VIF1 stalled in the UNPACK (STAT 10001083, FIFO full, VU1 idle). The EE User's Manual says ER0 detection is faulty: set VIFn_ERR.ME0 (and ME1 for V3-16). PCSX2 does not stop | maxsdk masks both at startup and on every VIF reset (`mx_vif_reset`) | `demos/levels.c` | 2026-10-23 |
| Strided UNPACK (STCYCL CL = item size, WL = 1) of each input stream into its slot of the items: V3-32, V4-8, V4-8U, V2-16 per vertex | correct for every batch size 1..max | `tests/ps2/shader.c` | 2026-10-23 |
