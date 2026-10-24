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
| vopmula → vopmsub → vopmula (cross product) | 5 (`bench/uarch_tests.c`, 2026-10-24) |
| vclipw | 1 per cycle |
| ACC ↔ VF copy (vmulaw acc, s, vf0w; vmsubx d, vf0, vf0x) | 5 round trip |
| vmove; vmove.x then vmove.xyzw of the same register | 4; 5 (the merge waits only for its field) |
| v4f32 select(a < b, x, y): MMI compare (clang) vs VU mask `clamp((b-a)·2^127)` + VMULA/VMADD blend | latency 17 vs 21, throughput 15.4 vs 18.1: the MMI path stays |

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

### Micro-mode instruction timing (PS2, 2026-10-24, `bench/vuuarch`)

Each probe body is unrolled 4 and 12 times in a VU loop; the difference of
the EE cycle counts gives the cycles per body (EE and VU share one clock).

| Fact | Value |
|---|---|
| FMAC result (ADD, MUL, MADD, bc forms, ADDi, ADDq, MAX, MINI, ABS, FTOI0/ITOF0) → FMAC | 4, interlocked |
| MOVE, MR32 (lower) → use | 4 |
| Dependences are tracked per field | ADD.x/.y/.z/.w on one register: 4 chains, 1 per cycle |
| ACC → ACC (MADDA chain), MULA → MADD | 1 |
| OPMULA → OPMSUB → OPMULA | 5 (1 + 4) |
| DIV, SQRT throughput; RSQRT | 7; 13. One divider: DIV then RSQRT 20; FMAC runs on during a divide |
| Q after DIV | **not interlocked**: a read up to 6 words after DIV sees the old Q, from 7 words the new; DIV, WAITQ, DIV 8; DIV → WAITQ → ADDq → DIV 12 |
| MAC flags (FMAND) and clip flags (FCAND, FCGET) | **not interlocked**: stale up to 3 words after the FMAC/CLIP, current from 4; no stall |
| LOI → ADDi | 1; an ADDi in the LOI's own word reads the old I |
| IADD, IADDIU → integer use | 1; IADD → MFIR 1 |
| ILW → address use, LQ → FMAC | 4 |
| ADD → SQ → LQ → ADD | 9 (store → load at the same address 1) |
| MTIR → MFIR → MTIR | 5 |
| Taken B: branch + delay slot + target | 3 words, no penalty |
| IBNE in the word right after the instruction that writes its VI | **reads the old VI** (the value before the writes of the last ~4 words, as PCSX2's `analyzeBranchVI`), no stall; one word later the new VI |
| VU1 EFU, issue → WAITP done (DIV's convention: DIV, WAITQ = 8 = 7 + 1) | ESADD 10, ESQRT/ESUM/ERCPR 11, ERSADD/ELENG/ERSQRT 17, ERLENG 23, ESIN 28, EEXP 43, EATAN 53 (all 1 below the VU target's table); ERCPR throughput 11 |
| EFU and FDIV | run in parallel: ERCPR + DIV with both waits 12 |

PCSX2 gives the same values for all value checks (Q, flags, branch VI).

## Phase 5 (PS2, 2026-10-23)

| Fact | Value | Source | Date |
|---|---|---|---|
| VU0 doubling kernel through VIF0, readback by COP2 vs by toSPR DMA from VU0 memory (`mx_vu0_read_dma`), batches of 240 | 29.8 vs 27.9 cycles/qw | `probe/p5_fast.c` | 2026-10-23 |
| vudemo, VU0 results read back by DMA too, all three units | 30.88 cycles/qw (30.39 / 31.00 before) | `bench/vudemo` | 2026-10-23 |
| vudemo through mx_sched (one pool: VU0 batches of 240, VU1 of 240, FPU chunks of 8): VU0 1200, VU1 1224, FPU 120 qw | **27.32 cycles/qw** (static schedule 30.88) | `bench/vudemo` MODE_SCHED | 2026-10-23 |
