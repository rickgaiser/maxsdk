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
| T-bit: stops VU1 and raises the VU1 interrupt with FBRST.TE1; VPU-STAT.VTS1 stays set until a reset or a resume (see VU states below). With TE1 off the T bit is passed over (a T + E kernel ends at the E bit) | | | `tests/ps2/vu.c`, vustate.c, p13_thold.c | 2026-10-24 |
| VU1 memory must not be read (mapped or by toSPR DMA) while VU1 runs: 104–396 of 1200 values wrong | | | `tests/ps2/vu.c` (first version) | 2026-10-23 |
| Double-buffered `mx_vuq` (BASE/OFFSET + FLG + xtop, next batch held at a VIF i-bit, DMA readback) | correct; 31.5 cycles/qw vs 31.0 single-buffered (batches of 120): no gain yet | | `probe/p4_vu.c` | 2026-10-23 |
| Batch completion: the DMA end can come before the VIF has seen the last quadwords (VIF looks idle, MSCAL not yet run); `mx_vuq` ends each batch with a VIF MARK and waits for it | | | `bench/vudemo` (VU0 by VIF0) | 2026-10-23 |
| XGKICK from xtop with VIF1 double buffering, 8 batches without FLUSH between them | works | | `tests/ps2/vu.c` | 2026-10-23 |

## VU states, VU0 and VU1 (PS2, 2026-10-24)

The Ready / Run / Stop states of VU User's Manual 1.3, measured with `tests/ps2/vustate.c`,
`tests/ps2/vustop.c` (programs in `tests/ps2/vustate.vsm`) and `probe/p12_vustate.c`. These
and a reverted library API for the states (state query, ForceBreak, resume, abort, recover)
are kept next to maxsdk in `maxsdk_vu_work_2026-10-24/`. Both VUs behave the same unless
noted.

| Fact | Value |
|---|---|
| VBS reads 0 in Stop as in Ready; only VDS/VTS/VFS tell them apart. `cfc2.i` (`mx_vu0_wait`) also returns when VU0 is in Stop (after 120 cycles) | **maxsdk (not fixed):** `mx_vuq_done`/`mx_vuq_wait` (COP2 path) take a stopped program for an ended one; behind a VIF the MARK waits spin 1 s and return -1 |
| ForceBreak from Ready and from Run: Stop with VFS; a second ForceBreak in Stop changes nothing | VBS clears 36 cycles after ForceBreak in Run |
| Resume from Stop: VCALLMS(R) (VU0), CMSAR1 (VU1); the VI registers are kept (a loop counter goes on) | |
| After a T bit: TPC = T address + 1; the delay slot has **not** run, so resume continues at the delay slot | |
| Stop → resume → E bit: **Ready**, the stop bits clear, the next MSCAL is taken. No reset needed | |
| A VIF MSCAL to a stopped VU waits (VIF STAT 0x06); a FLUSHE behind a ForceBroken program waits too. **A VU-only reset or a resume to an E bit lets the VIF go on** (MARK arrives); no VIF reset or DMA stop needed | |
| A VU reset keeps micro memory, data memory and the VI registers | |
| A VU reset clears that VU's own TE/DE even when the same FBRST write sets them; the other VU's are kept if written back. Set them again in a second write | |
| T bit with TE1 set and no VU1 handler installed: VU1 still stops (STOP(T)). VU0 started by VIF0 in the same case read VBS0 + VTS0 together (`0005`): not explained | |
| CMSAR1 written in Ready starts VU1 (the diagram's Ready → Run) | |
| D bit with DEn set: hangs the PS2 on VU1 and on VU0 (VCALLMS start, handler that masks itself after 100 calls) | vudbit.c (in `maxsdk_vu_work_2026-10-24/`) |
| INT_VU0WD (VU0 watchdog): no interrupt within 5 s of a spinning VU0, VFS0 not set | |

Costs (EE cycles, median of 9, `p12_vustate.c`):

| Primitive | Cycles |
|---|---|
| VPU-STAT read (cfc2) / FBRST read | 1.0 / 0.4 |
| uncached VIF1_STAT / VIF1_MARK read; CHCR read | 1.6; 1.0 |
| FBRST write (ctc2 + sync.p) | 9 |
| VU0: `vcallms imm` → end program → `cfc2.i` | 13 (poll VBS0 instead: 26) |
| VU0: `ctc2` CMSAR0 + 2 VNOP + `vcallmsr` → `cfc2.i` | 19 |
| VU1: CMSAR1 → end program → VBS1 clear | 31–62 |
| VU1: MSCAL + FLUSHE + MARK by VIF1 DMA, to the MARK | 136 |
| Start to VBS set: VCALLMSR (VU0) / CMSAR1 (VU1) | 12 / 19 |
| ForceBreak in Run → VBS clear | 36 |
| Resume at the end program → Ready: VU0 / VU1 | 26 / 32 |
| VU-only reset; VIF reset | 14; 14 |
| `mx_vu_reset` of one VU (VU + VIF) | 29–30 |
| DMA stop (EE manual 5.7) + VU + VIF reset | 108 |

Consequence: the per-batch overhead falls off above ~240 items; mapped readback (26 cycles/qw)
dominated, so `mx_vuq` can read VU1 back by DMA through the scratchpad (`mx_vuq_readback_spr`).

## vuk shaders (PLAN_PROCGEN P1/P2, PS2, 2026-10-23)

(The programs are HLSL now, [hlsl.md](hlsl.md); these numbers are of the vuk versions, measured on
the PS2.)

Compiled by vuk (`tools/vuk`, shader mode) and llc (VU target, feature tree), double-buffered
through the frame pipeline (`mx_frame_shader_draw`). Terrain patches of 33 x 33 vertices as one
strip each (2174 vertices, 124 degenerate join triangles), 64 patches (A, ~2 px triangles) or
16 patches (B, ~17 px, overdrawn), no vsync (`demos/procgen/levels.c`). EE cycles.

| Level | Per vertex | Loop of the kernel (static) | Measured A | Measured B | Busy (A) |
|---|---|---|---|---|---|
| L0 `lvl0.vuk` | position V3-32, colour V4-8 → RGBAQ, XYZ2 | 7.4 (37 words / 5 vertices, DIV bound 7) | **9.88** | 10.73 | VU1 93 %, GIF 73 %, GS 66 % |
| L1 `lvl1.vuk` | + normal V4-8, one light | 16 (ResMII 16) | **17.67** | 18.54 | VU1 99 % |
| L2 `lvl2.vuk` | + texture ST V2-16, perspective correct | 19 (ResMII 19) | **20.87** | 21.76 | VU1 100 % |
| L3 `lvl3.vuk` | 3 lights, fog, guard band / near clipping by ADC | 38 (ResMII 36) | **40.61** | 41.45 | VU1 100 % |

Batches: L0 125, L1 98, L2 70, L3 69 vertices; the batch costs ~1.7–2.5 cycles per vertex
above the loop (start, the stages before and after the kernel, the unrolled kernel's
remainder, XGKICK). L0 is at the GS's triangle rate (8 cycles per triangle, `docs/gif-gs.md`).
Per 60 Hz frame (4.2 M cycles usable): L0 ~425 k, L1 ~238 k, L2 ~201 k, L3 ~103 k vertices.

Correctness: every batch size 1..max of each level against the EE (`tests/ps2/shader.c`),
PS2 and PCSX2.

Later (PLAN_PROCGEN P4/P5, PS2, 2026-10-23), same measurement:

| Shader | Measured A | Measured B | Note |
|---|---|---|---|
| L3 with the ADC of the vertex and the two before it (shader `state`) | **50.59** | 51.40 | the clipping of a whole triangle: II 46 (ResMII 43) |
| `tf0.vuk` (procgen's far terrain: L0 + fog) | 13.94 → **13.33** | 14.84 → 14.23 | the second with `VUFoldInsertField` (the fog written into w in place: 11 → 10 upper instructions per vertex) |
| L1 / L2 / L3 with the colour clamped to 255 (RGBAQ keeps 8 bits: bright snow wrapped to red and yellow) and exact field copies (no multiply with 1.0 in fs, `docs/vu0.md`), 2026-10-24 | **18.62 / 23.10 / 53.71** | 19.47 / 23.98 / 54.52 | L0 9.89 and tf0 13.33 unchanged |

| Fact | Value | Conditions | Source | Date |
|---|---|---|---|---|
| VU1 waiting at XGKICK for the GIF (VPU-STAT.VGW1, sampled as `MX_TS_VU1GW`) | 0–6 % of the time for small triangles (A), 3–14 % with larger ones (B); 6 % in procgen's max count | | `demos/procgen/levels.c`, `demos/procgen/procgen.c` | 2026-10-23 |
