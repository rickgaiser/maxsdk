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
| **VIF1 stops with STAT.ER0 (DMAtag mismatch) on correct packets**: a V2-16 UNPACK of 72 vectors (18 qw, a `ref` tag with TTE in the MFIFO stream) left VIF1 stalled in the UNPACK (STAT 10001083, FIFO full, VU1 idle). The EE User's Manual says ER0 detection is faulty: set VIFn_ERR.ME0 (and ME1 for V3-16). PCSX2 does not stop | maxsdk masks both at startup and on every VIF reset (`mx_vif_reset`) | `apps/procgen/levels.c` | 2026-10-23 |
| Strided UNPACK (STCYCL CL = item size, WL = 1) of each input stream into its slot of the items: V3-32, V4-8, V4-8U, V2-16 per vertex | correct for every batch size 1..max | `tests/ps2/shader.c` | 2026-10-23 |

## UNPACK with skipping write (examples/vumax, PS2, 2026-10-25)

VIF1 normal-mode DMA from memory, 1024 vectors (UNPACKs of 256) into VU1 memory, VU1 idle,
EE cycles per vector, median of 8 (`probe/p14_unpack.c`). STCYCL CL = n, WL = 1 writes every
n-th quadword: the slot of one input in in-place vertices.

| Format | CL 1 | CL 2 | CL 3 | CL 4 | CL 2 WL 2 |
|---|---|---|---|---|---|
| V3-32 | 4.31 | 4.46 | 4.71 | 4.79 | 4.46 |
| V3-16 | 2.30 | 2.45 | 2.67 | 2.80 | 2.46 |
| V2-32 | 2.98 | 3.13 | 3.35 | 3.38 | 3.11 |
| V4-8 | 2.13 | 2.26 | 2.38 | 2.57 | 2.27 |
| V4-32 | 5.67 | 5.80 | 5.96 | 6.12 | 5.81 |

| Streams into one vertex (vumax) | Per vertex |
|---|---|
| V3-32 + V4-8, stride 2 | 6.46 |
| V3-16 + V4-8, stride 2 | 4.45 |
| V3-32 + V2-32 + V4-8, stride 3 | 9.63 |
| V3-16 + V2-32 + V4-8, stride 3 | 7.60 |

* A skipped quadword costs ~0.15 cycles; streams into the same vertices add up.
* STMOD offset mode (ROW added) costs nothing: V3-16 + ROW = 1.5 · 2^23 gives the VU floats
  1.5 · 2^23 + p for half the bytes of V3-32 (vumax c16; the MVP takes the offset out, ~1.5
  LSB of the 16-bit value lost).
* **While VU1 runs a kernel the VIF is slower**: vumax S3 (V3-16 + V2-32 + V4-8, a 7-cycle
  kernel with 2 LQ + 2 SQ per vertex) takes 11.28 cycles per vertex in a frame and 11.31 in a
  plain chain, against 7.60 VIF-only: ~50 % more (not measured in isolation: suspected VU
  memory port sharing).
* STMOD stays set: an UNPACK after a STMOD 1 stream (a header, the uniforms) gets ROW added
  too; vumax resets it before every batch and context.
