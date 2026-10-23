# DMAC

| Fact | Value | Conditions | Source | Date |
|---|---|---|---|---|
| Tags, MADR and TADR need **physical** addresses. Passing the uncached alias (0x2…) of the tags **crashed the PS2**; PCSX2 masks it. | | VIF1 chain | `bench/vudemo` | 2026-10-23 |
| VIF0 chain DMA with the tags written through the uncached alias **starts and works** in isolation (`CHCR 70000045`, data arrived), and so does the cached + written-back version | | chain + TTE, 1 REF + END | `probe/p2_dma.c` | 2026-10-23 |
| (Earlier, in the ps2sdk vudemo, VIF0 chain DMA with uncached tags and parameters did not start; the cause was not found. maxsdk writes VIF0 tags cached and writes them back.) | | | `bench/vudemo` (ps2sdk version) | 2026-10-23 |
| toSPR, memory → SPR, 16 KiB | 2.41 / 2.46 cycles/qw | normal mode | `probe/p2_dma.c` | 2026-10-23 |
| fromSPR, SPR → memory, 16 KiB | 2.38 / 2.40 cycles/qw | | `probe/p2_dma.c` | 2026-10-23 |
| VIF1, 16 KiB of VIF NOPs from memory | 4.70 / 4.75 cycles/qw | normal mode | `probe/p2_dma.c` | 2026-10-23 |
| VIF1, 16 KiB of VIF NOPs from SPR | 4.02 / 4.02 cycles/qw | | `probe/p2_dma.c` | 2026-10-23 |
| VIF0, 16 KiB of VIF NOPs from memory | 7.57 / 7.59 cycles/qw | | `probe/p2_dma.c` | 2026-10-23 |
| VIF1, 512 qw of NOPs in one transfer | 2417 / 2441 cycles | | `probe/p2_dma.c` | 2026-10-23 |
| The same as 32 segments of 16 qw through the interrupt-refilled queue (`mx_dmaq`) | 18810 / 18832 cycles: **+529 cycles per segment** | | `probe/p2_dma.c` | 2026-10-23 |
| Of that, our handler from entry to the next segment's start | 100 / 102 cycles | | `probe/p2_dma.c` | 2026-10-23 |
| MFIFO: fromSPR → 8 KiB ring → VIF1, 64-qw packets (cnt + 63 NOPs), incl. the EE's push loop | 7.94 / 7.97 cycles/qw | | `probe/p2_dma.c` | 2026-10-23 |
| MFIFO: `mx_mfifo_push` returns while fromSPR still reads the SPR source. Rewriting the source at once corrupted 53 of 192 words on the PS2 (fine on PCSX2): double-buffer the SPR source (`mx_mfifo_push_wait`) | | | `tests/ps2/dma.c` (first version) | 2026-10-23 |
| **toSPR DMA can read VU1 data memory** (MADR = 0x1100c000): 960 qw in 3.10 cycles/qw, data correct | 8× faster than mapped lq/sq (26.07 cycles/qw) | VU1 stopped | `probe/p4_vu.c` | 2026-10-23 |
| VU1 per batch in the ps2sdk vudemo: sending by DMA | ~2400 EE cycles per batch | REF chain, UNPACK, MSCAL, FLUSHE | `bench/vudemo` | 2026-10-23 |
| Stall control, fromSPR (source) → VIF1 (drain) with a `refs` packet on a chain drain: the drain waits for the source | works | | `tests/ps2/dma.c` | 2026-10-23 |

## Not measured yet

* Stall control with a normal-mode drain on the PS2 (PCSX2 does not stall it).
* Channel priorities (D_PCR) and cycle stealing (D_CTRL.RELE) under load.
* Fence methods other than the per-segment interrupt (VIF MARK, a DMA write into SPR).
