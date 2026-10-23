# VU1

| Fact | Value | Conditions | Source | Date |
|---|---|---|---|---|
| Feeding VU1 memory through the memory map (0x1100_c000) + CMSAR1 start gave wrong results on the PS2 | | | `bench/vudemo` | 2026-10-23 |
| Working control: VIF1 DMA with REF tags to the data, UNPACK, MSCAL, **FLUSHE** | | | `bench/vudemo`, `tests/ps2/vu.c` | 2026-10-23 |
| `FLUSH` also waits for the GIF and stalls VIF1 (STAT.VGW); `FLUSHE` only waits for the micro program | | no PATH1 traffic | `bench/vudemo` | 2026-10-23 |
| Reading results back uncached from VU1 memory (lq + sq to UCAB) | 26.05 / 26.07 cycles/qw | VU1 stopped | `probe/p4_vu.c` | 2026-10-23 |
| **Reading VU1 memory by toSPR DMA** | 3.10 cycles/qw | VU1 stopped | `probe/p4_vu.c` | 2026-10-23 |
| `mx_vu1_read_dma`: toSPR from VU1 memory + fromSPR to memory (UCAB), 960 qw | 5.47 / 5.51 cycles/qw | | `probe/p4_vu.c` | 2026-10-23 |
| vudemo VU1 alone with that readback | 38.96 cycles/qw (mapped reads 59.23, ps2sdk version 61.8) | | `bench/vudemo` | 2026-10-23 |
| XGKICK placement, 8 full-height sprites: one packet kicked once vs 8 packets kicked one by one, MSCAL to FINISH | 1013 vs 1003 cycles (no difference: the GS fill dominates) | | `probe/p4_vu.c` | 2026-10-23 |
| A doubling kernel through `mx_vuq` (send + run + mapped read), 960 items in batches of 30 / 60 / 120 / 240 / 480 / 960 | 71.7 / 53.8 / 48.3 / 45.6 / 44.4 / 43.8 cycles/qw | | `probe/p4_vu.c` | 2026-10-23 |
| VU1 alone, vudemo poly D=16, maxsdk | 59.23 cycles/qw (ps2sdk version 61.8) | | `bench/vudemo` | 2026-10-23 |
| T-bit: stops VU1 and raises the VU1 interrupt with FBRST.TE1; VPU-STAT.VTS1 stays set until a reset | | | `tests/ps2/vu.c` | 2026-10-23 |
| VU1 memory must not be read (mapped or by toSPR DMA) while VU1 runs: 104–396 of 1200 values wrong | | | `tests/ps2/vu.c` (first version) | 2026-10-23 |
| Double-buffered `mx_vuq` (BASE/OFFSET + FLG + xtop, next batch held at a VIF i-bit, DMA readback) | correct; 31.5 cycles/qw vs 31.0 single-buffered (batches of 120): no gain yet | | `probe/p4_vu.c` | 2026-10-23 |
| Batch completion: the DMA end can come before the VIF has seen the last quadwords (VIF looks idle, MSCAL not yet run); `mx_vuq` ends each batch with a VIF MARK and waits for it | | | `bench/vudemo` (VU0 by VIF0) | 2026-10-23 |
| XGKICK from xtop with VIF1 double buffering, 8 batches without FLUSH between them | works | | `tests/ps2/vu.c` | 2026-10-23 |

Consequence: the per-batch overhead falls off above ~240 items; mapped readback (26 cycles/qw)
dominated, so `mx_vuq` can read VU1 back by DMA through the scratchpad (`mx_vuq_readback_spr`).
