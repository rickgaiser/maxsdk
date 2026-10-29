# Memory, caches and the scratchpad

| Fact | Value | Conditions | Source | Date |
|---|---|---|---|---|
| `mx_dcache_writeback` (CACHE DHWOIN per 64-byte line) | 1 KiB 370, 4 KiB 1426, 16 KiB 3738 cycles (~23 per line) | all lines dirty | `probe/p2_dma.c` | 2026-10-23 |
| BIOS `FlushCache(0)` (whole data cache) with 16 KiB dirty | 2877 / 2895 cycles | | `probe/p2_dma.c` | 2026-10-23 |
| → for more than ~12 KiB, flushing the whole cache is cheaper than per-line write back | | | | |
| `sq` stream (4 per 64 bytes), 8 KiB: cached, hot | 1.40 cycles/qw | | `probe/p2_dma.c` | 2026-10-23 |
| … to uncached memory (0x2…) | 6.15 / 6.23 cycles/qw | | `probe/p2_dma.c` | 2026-10-23 |
| … to UCAB memory (0x3…) | 6.16 / 6.23 cycles/qw (no gain for a pure 128-bit store stream) | | `probe/p2_dma.c` | 2026-10-23 |
| … to the scratchpad | 1.26 cycles/qw | | `probe/p2_dma.c` | 2026-10-23 |
| `SyncDCache` (BIOS) per batch in the ps2sdk VU demo | ~8k EE cycles | a few KiB per call | `apps/llvm_bench/vudemo` | 2026-10-23 |
| Load misses never overlap | 2 misses to different lines 104 cycles, 1 miss 37 | | `apps/llvm_bench/uarch_tests.c` | 2026-10-21 |
| A PREF miss overlaps a load miss fully | | | `apps/llvm_bench/uarch_tests.c` | 2026-10-21 |
| PREF placement in a loop changes speed by up to 70%, not monotonically | | no safe rule found yet | `bench` branch `prefetch` | 2026-10-21 |
| In-cache `x*3+1` per 64-byte line | scalar 53, MMI aligned 19, MMI unaligned (pcpyld) 54 cycles | | `apps/llvm_bench/uarch_tests.c` | 2026-10-20 |
| VU0 macro mode in the demo: 28 cycles/qword static, 36.6 measured; the gap is data cache misses (one line per 4 qwords) | | | `apps/llvm_bench/vudemo` | 2026-10-23 |

Consequence for packets (D6): build per-batch packets in the scratchpad (1.26 cycles/qw) and send
them from there; uncached and UCAB stores are ~5x slower.
