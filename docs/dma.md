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

## Design facts from phases 5-7 (from the manual, checked on PCSX2)

* In MFIFO mode fromSPR is the ring's source and its MADR is the ring's write pointer. Any other
  fromSPR transfer (e.g. the DMA readback of VU memory, toSPR + fromSPR) moves that pointer and
  breaks the ring: `demos/allunits/allunits.c` hung on PCSX2 until the readback copied from the scratchpad
  with the EE instead while the MFIFO runs (`src/vu/vu.c` read_dma).
* An MFIFO drain (VIF1) in source chain mode with TTE reads `ref` tags that point outside the ring
  (vertex arrays in memory) and sends the VIF codes in each tag: the frame pipeline relies on both
  (`tests/ps2/frame.c`, PCSX2; PS2 pending).
* MPG from `ref` tags straight to the code (`mx_vu_load`, `mx_vu_mpg_tags`): the MPG code sits in
  the tag's upper word, so the instructions start on a quadword; an odd last instruction goes in a
  `cnt` packet padded with VIF NOPs (`tests/ps2/vu.c` mpg_big: 601 instructions, PCSX2).

## Not measured yet

* Channel priorities (D_PCR) beyond the three-channel run above.

* Stall control with a normal-mode drain on the PS2 (PCSX2 does not stall it).
* Channel priorities (D_PCR) and cycle stealing (D_CTRL.RELE) under load.
* Fence methods other than the per-segment interrupt (VIF MARK, a DMA write into SPR).

## Phase 5 (PS2, 2026-10-23)

| Fact | Value | Conditions | Source | Date |
|---|---|---|---|---|
| VIF1 1-qw chain, DMA start until the EE knows it is done: DMA end interrupt (mx_dmaq) | 751 / 763 cycles | | `probe/p5_fast.c` | 2026-10-23 |
| … polled VIF1 MARK | **100** cycles | | `probe/p5_fast.c` | 2026-10-23 |
| 64 batches of 16 qw: a queue segment (interrupt) per batch | 561 cycles/batch | | `probe/p5_fast.c` | 2026-10-23 |
| … one chain, a MARK per batch, polled | **121** cycles/batch | | `probe/p5_fast.c` | 2026-10-23 |
| MPG of 1024 instructions by ref tags to the code (`mx_vu_load`) | 3.10 cycles/instruction (copying builder: 25) | | `probe/p4_vu.c` | 2026-10-23 |
| Stall control with the drain (VIF1) in **normal** mode: the drain waits for the source; data correct (PCSX2 does not stall it) | | | `probe/p5_fast.c` | 2026-10-23 |
| 2048 uncached EE stores alone / while VIF1 reads 16 KiB from memory | 12.9 k / 18.1 k cycles; **VIF1 slows from 4.5 k to 18.2 k cycles** | D_CTRL.RELE off | `probe/p5_fast.c` | 2026-10-23 |
| … with RELE (release every 8 / 64 cycles) | 18.1 k / 17.7 k: cycle stealing barely changes it | | `probe/p5_fast.c` | 2026-10-23 |
| toSPR 16 KiB, VIF1 16 KiB, GIF 64 KiB IMAGE started together (end times) vs alone | toSPR 2.77 k (2.61 k), VIF1 9.73 k (4.53 k), GIF 23.3 k (19.7 k) | | `probe/p5_fast.c` | 2026-10-23 |

Consequences: poll the VIF MARK to wait for a batch (D13); put many batches in one chain
with a MARK each instead of a queue segment per batch; the EE's uncached stores and VIF1 DMA from
memory slow each other a lot, so build packets in SPR, not uncached memory, while DMA runs.
