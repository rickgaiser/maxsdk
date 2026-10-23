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
