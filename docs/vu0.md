# VU0

## Macro mode (COP2), measured with `bench/uarch_tests.c` (2026-10-21)

| Fact | Value |
|---|---|
| vadd, vmul, vmadd, vmax latency | 4 |
| ACC → ACC (vmula + vmadd chain) | 1 (vmula + vmadd 5) |
| qmtc2 / lqc2 → vadd | 2 |
| vadd → qmfc2 / sqc2 | reads late, +1 |
| vdiv / vsqrt → vwaitq | 9; vrsqrt 15; vdiv does not block vadd |
| One COP2 op per cycle | vadd pairs with addu and lw, not with lqc2, mul.s or paddw |
| Multiply with 1.0 in fs (`vmulw.w d, vf0, s`) | 1 ULP towards 0 for any value but a power of two; 1.0 in ft is exact (`probe/p10_vucopy.c`, 2026-10-24) |
| Add of operands of very different size | the smaller is cut to one bit below the sum's last before the add: `(v + 1.5·2^23) − 1.5·2^23` is floor(v) + 1 for a negative v with a fraction below 0.5 (`tests/ps2/floor.c`, 2026-10-24) |
| Macro and micro mode in one program | a plain macro instruction (VADD) issued while a micro program runs waits for it to end (196 876 cycles after a VCALLMS of fbm), and the program's results are unchanged; QMFC2.I the same; a VF register set by macro code is overwritten by the program (they are shared). PCSX2 the same. (`probe/p11_vu0mix.c`, 2026-10-24) |
| Compiled macro code (clang `-mvu0`) | VF registers are caller-saved (the VU0 callee-saved list in MipsCallingConv.td is not used), so no VF value is kept across a call such as `mx_vuq_batch`; inline asm that starts a program names them as clobbered (`"$vf1"`…`"$vf31"`, added to clang 2026-10-24); `cfc2.i`/`ctc2.i`/`qmfc2.i`/`qmtc2.i` assemble with `-mvu0` |
| lqc2 / sqc2 | 1 per cycle (load/store unit), pair with addu, not with lw |
| qmtc2 / qmfc2 | 1 per cycle, pair with addu/paddw, not with lw/mtc1/vadd |
| qmfc2 → GPR | 3; qmfc2 → mtc1 → add.s 4.5; pextlw → qmtc2 → vadd 4 |
| sw → sqc2 forwarding | 3.5 |
| Unaligned load to vadd | 9 (ldl/ldr + pcpyld + qmtc2) vs 11 via the stack |
| vftoi0 / vitof0 / vmr32 | latency 4, 1 per cycle |

## Micro mode

| Fact | Value | Source | Date |
|---|---|---|---|
| Writing VU0 memory through the memory map (0x1100_4000) and starting with VCALLMS gave wrong results on the PS2 (fine on PCSX2 and with delays) | | `bench/vudemo` | 2026-10-23 |
| What works: data in with `lqc2` + `vsqi`, out with `vlqi` + `sqc2`, start with `vcallms`, wait with `cfc2.i` | EE cost per qword in the demo: 17 to write, 8.5 to read | `bench/vudemo` | 2026-10-23 |
| `mx_vu0_write` (lqc2 + vsqi), 240 qw, data in the cache | 6.04 / 6.05 cycles/qw | `probe/p4_vu.c` | 2026-10-23 |
| `mx_vu0_read` (vlqi + sqc2) to UCAB, 240 qw | 8.05 cycles/qw | `probe/p4_vu.c` | 2026-10-23 |
| VU0 micro mode fed by **VIF0 DMA** (maxsdk `mx_vuq`, tags + header written cached and written back) is faster than COP2 in vudemo: VU0 alone 45.79 vs 49.66 cycles/qw, all three units 31.63 vs 35.56 | | `bench/vudemo` (`VU0_DMA` 1 vs 0) | 2026-10-23 |
| VU0 memory must not be read (COP2 or mapped) while VU0 runs: double-buffered batches read while the next batch ran gave 951 of 1200 wrong values. COP2 transfers also change vi2/vi3 of a running program (`ctc2` does not wait). | | `tests/ps2/vu.c` (first version) | 2026-10-23 |
| Double-buffered `mx_vuq` (ITOP + xitop ping-pong, next batch held at a VIF i-bit until the previous one is read, mapped readback) | correct; 74.4 cycles/qw vs 44.7 single-buffered (doubling kernel, batches of 60): the per-batch interrupt, the hold and the mapped reads cost more than the overlap gains | `probe/p4_vu.c` | 2026-10-23 |
| VIF0 ITOP read by VU0 with `xitop`, MPG upload to VU0 through VIF0 | work | `tests/ps2/vu.c` | 2026-10-23 |
| The assembler rejects `lqc2`/`sqc2`/`vcallms`/`cfc2.i` without `-mvu0`; they are encoded as `.word` (vcallms 0x4a000038, cfc2.i 0x4848e801) | | `bench/vudemo/vu.h` | 2026-10-23 |

Micro-mode instruction latencies are not measured yet; the VU target's model uses the manual's.

## Phase 5 (PS2, 2026-10-23)

| Fact | Value | Source | Date |
|---|---|---|---|
| VU0 doubling kernel through VIF0, readback by COP2 vs by toSPR DMA from VU0 memory (`mx_vu0_read_dma`), batches of 240 | 29.8 vs 27.9 cycles/qw | `probe/p5_fast.c` | 2026-10-23 |
| vudemo, VU0 results read back by DMA too, all three units | 30.88 cycles/qw (30.39 / 31.00 before) | `bench/vudemo` | 2026-10-23 |
| vudemo through mx_sched (one pool: VU0 batches of 240, VU1 of 240, FPU chunks of 8): VU0 1200, VU1 1224, FPU 120 qw | **27.32 cycles/qw** (static schedule 30.88) | `bench/vudemo` MODE_SCHED | 2026-10-23 |
