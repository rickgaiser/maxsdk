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

## Draw contexts (PS2, 2026-10-25)

HLSL vertex programs take the GIF tag and the uniforms from a context that the EE writes once
per draw (llvm `VUEntryABI.md`), not from every batch. EE cycles; `demos/procgen/levels.c`
(scene A) and `demos/model/model.c` (8 x 8 copies, no vsync), the same build otherwise.

| | Uniforms in every batch | Two contexts (default) | One context (FLUSHE per draw) |
|---|---|---|---|
| levels L0 / L1 / L2 / L3 / tf0, cycles per vertex | 9.67 / 18.59 / 21.96 / 49.50 / 12.23 | 9.62 / 18.64 / 22.02 / 49.57 / 12.27 | |
| model mflat strips: fps, EE busy | 463.6, 31 % | 462.6, 27 % | 434.2, 25 % |
| model mskin strips (104 uniform qw): fps, EE busy | 21.9, 13 % | 21.9, 7 % | 21.8, 6 % |
| model mskin indexed: fps | 46.3 | 46.3 | 45.7 |
| mskin strip vertices per batch | 48 | 48 | 55 |

Consequence: VU1 is the limit in all of these (99 %), so the frame rate stays; the EE no longer
copies the uniforms into every batch (mskin strips: half the EE time). One context gives the
buffers more room but costs up to 6 % (the pipeline drains at every draw): two is the default.

Then (stage 2) the program loads the uniforms once per draw: MSCAL for a draw's first batch (the
init, then the batch), MSCNT for the others, which continue after the E bit of the batch before
with its registers (llvm `VUEntryABI.md`):

| | Two contexts (above) | + init once per draw |
|---|---|---|
| levels L0 / L1 / L2 / L3 / tf0, cycles per vertex | 9.62 / 18.64 / 22.02 / 49.57 / 12.27 | 9.64 / 18.59 / 21.99 / 49.41 / 12.24 |
| model mflat / mlit / mtex / mskin strips, fps | 462.6 / 77.9 / 76.6 / 21.9 | 463.7 / 78.0 / 76.7 / 21.9 |
| model mflat / mskin indexed, fps | 273.2 / 46.3 | 274.5 / 45.7 |

The batch start is 9 words instead of ~15 (lvl1), which is within the noise here: the pipelined
loop's prologue and epilogue (lvl1 ~90 words around a kernel of 17) dominate the cost of a batch.

Found on the way (PS2):
* An MSCAL of the init alone, before the first batch, swaps the VIF1 buffers too: the first batch
  then wrote the buffer whose packet the GIF still read. An XGKICK of an empty tag in the init
  (waiting for the GIF) made it correct but cost 4-6 % (VU1 waited for the GIF at every draw), so
  the init runs as part of the first batch.
* With the batches a loop around the item loop, block placement rotated it so that the item loop
  jumped to its exit, which the delay slot filler and the loop scheduler do not take: lvl1 22.86
  cycles per vertex. llvm `VULoopLayout` moves the exit back behind the item loop.

## Vertex rate at the floor: vumax (PS2, 2026-10-25)

`demos/vumax/vumax.c`: hand-scheduled kernels (`tools/vusched/vusched.py`, `vx_*.vsm`) next to the
same programs in HLSL (`vh_*.hlsl`). The floor of a kernel is its upper (FMAC) ops per vertex;
S1/S2 would be DIV-bound at 7 but take every second 1/w from the EFU (ERCPR), 4 vertices per
24 cycles. In-place batches (the VIF puts each input into the quadword of its GIF register;
colours never pass through VU registers), triple-buffered, no prologue/epilogue code (the loop
runs S − 1 iterations more and stores into a pad and a slop). levels.c's scene A: 64 patches
of 2174 strip vertices per frame, no vsync, EE cycles per vertex; c16 = V3-16 positions by
STMOD offset (`docs/vif.md`), f32 = V3-32.

| | Floor | Kernel alone | vsm c16 no XGKICK | vsm f32 no XGKICK | vsm c16 | vsm f32 | HLSL f32 | ps2gl (VCL, static) |
|---|---|---|---|---|---|---|---|---|
| S1 flat (XYZ2) | 6 | 5.92 | **6.55** | 6.81 | 7.00 | 7.09 | 7.70 | |
| S2 Gouraud (+ colour) | 6 | 5.94 | **7.01** | 8.05 | 8.44 | 8.58 | 9.65 | |
| S3 tex (+ ST) | 7 | 7.04 | 11.28 | 11.96 | 12.70 | 13.02 | 14.60 | fast_nolights 12 |
| S3b tex, ST V2-16 + ITOF12 | 8 | 7.92 | 10.88 | 11.59 | 12.34 | 12.50 | 14.22 | |
| S4 lit (1 light) | 11 | 10.98 | **11.93** | 12.04 | 12.25 | 12.33 | 17.55 | |
| S5 lit + tex | 12 | 12.02 | **13.34** | 14.22 | 15.39 | 15.69 | 19.66 | fast 16 |
| S6 SCEI (3 lights, clamp, ST, ADC clip) | 17 | 17.02 | **18.23** | 18.30 | 18.61 | 18.62 | 46.37 | scei 19 |

"Kernel alone": the batch already in VU1 memory, MSCAL to the end, slope between 8·u and
the full batch (`vumax k`); every kernel runs at its schedule. All 14 kernel/format pairs agree
with the EE for every batch size (vumax part 1, PS2 and PCSX2).

What limits each (busy shares of the same runs):
* S1, S4, S6 (VU1 95–100 %): the batch: S − 1 extra kernel iterations (S = 3–4 stages) and
  the program start, 0.5–1.2 cycles per vertex at 100–300 vertices per batch.
* S2 with XGKICK: the GS (8 EE cycles per Gouraud triangle, GIF 98 %); without it 7.01.
* S3, S3b, S5, and every f32 case: the VIF (VU1 60–88 %). The VIF is ~50 % slower while VU1
  runs (`docs/vif.md`), so S3's three streams (7.6 VIF cycles alone) take 11.3. Compressed
  inputs help everywhere (c16 vs f32); ST as V2-16 gains 0.4 for an extra upper op.
* HLSL: S1–S5 at 1.1–1.5× the hand kernels; S6 at 2.5× (46.37). Since then S4–S6 are within
  1.04–1.12× (next section).

Found on the way:
* An EE bug looked like a VU limit: the demo computed the batch size per draw with a loop of
  integer divides (2 M cycles per frame), and VU1 sat 25–60 % idle. Check "EE in frame()"
  before blaming the units.
* The VU pipeline model of vusched (Q new from DIV + 7, P from ERCPR + 12, no interlock on
  either; reads stall while a write to the register is in flight; ACC chains back to back)
  matches the PS2: the kernels run at II/u with no stall.

Per 60 Hz frame (4.92 M cycles) without the GS: S1 ~750 k, S2 ~700 k, S4 ~410 k, S6 ~270 k
vertices; with it, S2 is GS-bound at ~580 k.

## HLSL instruction selection after vumax (PS2, 2026-10-25)

The HLSL twins of vumax against the hand kernels: what the compiler did with the instructions of
the loop, and the fixes (llvm feature tree, `lib/Target/VU`). EE cycles per vertex, f32, with
the GS (the "HLSL f32" column above); upper (FMAC) instructions per vertex in the loop.

| | Hand kernel (upper) | HLSL before (upper) | HLSL after (upper) | vsm f32 | HLSL f32 before | HLSL f32 after |
|---|---|---|---|---|---|---|
| S1 flat | 6 | 6 | 6 | 7.09 | 7.70 | 7.70 |
| S2 Gouraud | 6 | 6 | 6 | 8.57 | 9.65 | 9.65 |
| S3 tex | 7 | 8 | 8 | 13.03 | 14.60 | **14.44** |
| S4 lit | 11 | 16 | **11** | 12.33 | 17.55 | **12.79** |
| S5 lit + tex | 12 | 18 | **12** | 15.71 | 19.66 | **15.66** |
| S6 SCEI | 17 | 37 | **18** | 18.62 | 46.37 | **19.96** |

What it took (each a general rule, with a lit test):
* `float4(v.xyz, a.w)`: four field insertions (three ADDs with vf0 and a MULw) became one lower
  MOVE.w (`MERGE_FIELDS`, `merge-fields.ll`): the vector that gives the most fields is the start.
  In a loop, with `a` from outside of it and the result only stored, not even that: the ADD writes
  x, y and z into a register that goes around the loop, whose w is set before it
  (`VUInvariantFields`, `invariant-fields.ll`; S6 20.94 → 19.96, its loop 18 words, all upper).
  Not when more instructions read the result: it can then live into the next iteration, and the
  register is copied in every iteration (model MLIT 95.3 → 91.5 fps when tried). S5 is 15.49
  without it, 15.66 with it, although its loop is a word shorter: VU1 is 77 % busy there, the
  VIF is the limit.
* The colour's FTOI0: `vu::rgba8` (vu.hlsli) adds 1.5 · 2^23, and the add joins the ambient
  colour and the clamp by 255 (`magic-add.ll`): amb + M is computed before the loop and added
  last (first, it rounded every sum after it: one colour of S6 was 2 off), min (x, 255) + M is
  min (x + M, 255 + M).
* ST: w is not read by the GIF and is no longer computed (MR32 gone); `float3 st :
  TEXCOORD_V2_32` has z = 1.0 from the VIF (STMASK, COL), so `float4(st * q, 0)` is one MULq.
  The masked UNPACK costs the VIF time: S3 (VIF-bound) 15.21 with it, 14.44 without, so its twin
  keeps `float2 st`; S5 15.49 with, 15.56 without (VU1 98 %).
* S6's clipping: `vu::clip` / `vu::clipped` / `vu::adc` (CLIPw, FCAND, bit 15) instead of
  ten FMAC instructions and a static history (`clip.ll`). A CLIP and its test are one
  instruction for the pipeliner (otherwise they ended in different stages, which the schedule
  check rejects: II 28 for ResMII 20); the clipping flags have no interlock, VUHazards puts
  empty words before a test closer than 4 cycles to its CLIP.
* S6 has one upper instruction more than the hand kernel: `t + prm` before FTOI4 (the hand
  kernel adds the offset with the magic number, which leaves 0x4B4 in the high bits of Z).

Other programs, the same day: levels L0 9.63, L1 18.59, L2 22.07, L3 49.41 → **47.49**, tf0
12.24 cycles per vertex; model MLIT strips 78.0 → **95.3** fps, MFLAT 463.7, MSKIN 21.9 (the
same), MTEX 76.7 → 75.9 (the same pipelined II 45; the post-RA schedule of its 5× unrolled loop
is 7 words longer, 227 against 220). Still to do (the plan's phases 2 and 3): the S − 1
iterations of prologue and epilogue per batch, in-place triple buffers, DIV and ERCPR by turns
for S1/S2, compressed inputs.
