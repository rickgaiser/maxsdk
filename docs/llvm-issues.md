# LLVM issues found by the model demo (2026-10-25)

Found while moving the model demo's skinning to the EE (VU0 macro mode, clang `-mvu0`) and
then to VU0 micro mode (HLSL compute). Toolchain: the feature tree (`r5900-feature`,
`toolchain/build/llvm`, llvm-project e99c87ba2021). The reproducers are in `docs/llvm-issues/`.

| # | Target | Kind | Effect | Status (2026-10-25 22:00 build) |
|---|---|---|---|---|
| 1 | VU (HLSL) | crash / cannot select | **blocks** the VU0 skinning program: any `uint` `/ 4`, `>> n` | fixed |
| 2 | VU (HLSL) | unchecked, behind 1 | what the skinning program needs next | checked 2026-10-26 (docs/llvm-issues/vskin.hlsl, with `asint` for the weights): all items done: LQ + ITOF0 for the VIF ints (2 a vertex), FTOI0 + SQ in VF, ILW + LQ for the palette, packing as MR32/MOVE in the lower pipe, stores past n. Left: the loop speed, 182 words for 4 vertices (45.5 a vertex, 145 upper ops), not pipelined: see 12 |
| 3 | Mips `-mvu0` | slow code | a scalar into a VU0 vector goes through the stack | fixed (`mfc1` + `qmtc2`) |
| 4 | Mips R5900 (MMI) | slow code | `v4i32` from four GPRs goes through the stack (also without `-mvu0`) | fixed (`pextlw` + `pcpyld`) |
| 5 | Mips R5900 (MMI) | slow code | `u8x4`/`s8x4` → `v4i32` goes byte by byte through the stack (also without `-mvu0`) | fixed (`pextlb`/`pextlh`, `psraw`; `ppach`/`ppacb` back) |
| 6 | Mips `-mvu0` | slow code | storing lanes of a VU0 vector goes through the FPU | fixed (`qmfc2` + `sw`) |
| 7 | VU (HLSL) | slow code | `mskin` (VU1): 112 cycles a vertex, one vertex per loop iteration | improved (af4807c366f1): the pipeliner's II 85 stalled after register allocation (89 words, 101 cycles); VULoopScheduler now bisects the lengths its step budget did not reach: 88 words in 88 cycles a vertex. PS2 (FOX, 8 x 8 copies): MSKIN strips 37.6 -> 43.1 fps, indexed 72.2 -> 79.8 fps, same pictures |
| 8 | VU (HLSL) | error | **blocks** V3-16 positions in `mskin_ix`: one more VF register than 31, no spill room | fixed; mskin uses V3-16 now |
| 9 | VU (HLSL) | slow code | `float3(int3) * (1.0 / 32768.0)` is ITOF0 + MUL, not ITOF15 | fixed |
| 10 | VU (HLSL) | slow compile | llc takes 122 s for `vskin.hlsl` (one VU0 program) | fixed (4dab979e941b): 9.5 s (was 122 s, 44 s after 22:00). All of it was the pipeliner trying II 151-191, each schedule needing 24-150 registers beyond the limit when the lifetime budget ran out; the II search now stops after 8 such IIs in a row (`-pipeliner-lifetime-budget-iis`). Same code for all 35 maxsdk programs |
| 11 | VU (HLSL) | slow code | vskin: 31 lane inserts in the upper pipe a group (MR32 / MOVE in the lower would do) | partly after 22:00: 36 → 28 upper moves; MR32/MOVE where the lower pipe has room |
| 12 | VU (HLSL) | slow code | vskin: the group loop is not software pipelined: 46 cycles a vertex, ~31 upper ops | not pipelinable as written: at every II up to 191 the overlap of two groups (four vertices each) needs far more than the 28 VF registers, and the unpipelined loop is 209 words. One vertex an iteration (or two) would be the way; that is the shader's structure, not the compiler |
| 13 | Mips `-mvu0` (MachinePipeliner) | **wrong code** | the EE skinning loop of one joint without normals, software pipelined, writes wrong positions (2084 of 99,673 vertices) | not the compiler: PCSX2's EE recompiler. With the EE interpreter (`EnableEE = false`) the same ELF gives 0 differences; the trigger is MipsR5900Issue's order of the kernel (`pextlh $14, ...` right after `qmtc2 $14, $vf15` after a VMADDA), which is equivalent in program order. Confirmed on the PS2 (2026-10-26): `m13_p.elf` gives `micro normals 0 vs 1: 0 positions differ` |
| 14 | clang | pragma lost | `#pragma clang loop unroll_count(2) pipeline(disable)` drops the `pipeline(disable)` (also as two pragmas) | fixed (f6def3308e3b): `pipeline.disable` is in the unrolled loop's `followup_all` |
| 15 | Mips `-mvu0` | slow code | `127 / sqrt(dot)` of a VU0 vector goes out to the FPU and back (QMFC2, MTC1, SQRT.S, DIV.S, MFC1, QMTC2): ~3 of 16 ms of EE skinning a frame | fixed with afn (cdd3e5c6f6b0); the model Makefile now builds model.c with `-mvu0 -fapprox-func` |
| 16 | Mips `-mvu0` (asm) | missing instructions | `vcallms`, `vcallmsr`, and `cfc2`/`ctc2` with VU0 control or VI registers are not accepted; micro mode needs them for every call (`.word` for now) | fixed (16d6b0e05b1e): VCALLMS (byte address, multiple of 8, below 2^18) and VCALLMSR; `$vi0`..`$vi31` are accepted as COP2 control registers by CFC2/CTC2 (.i too). Same encodings as binutils (cfc2.i $8, $vi29 = 0x4848e801) |
| 17 | clang `-mvu0` | missing feature | no way to keep VF registers reserved across asm statements (`-ffixed-vfN` or a register variable): a register-fed VU0 micro program's inputs and results stay in vf1..vf12 between the calls | fixed (755d1464feb7): `-ffixed-vf1` .. `-ffixed-vf31` (target feature `+reserve-vfN`): the registers are not allocated, and the VF pressure limit of the schedulers counts them out |
| 18 | Mips R5900 (MMI) | slow code | four bytes zero-extended to words: the halfword step goes through the stack (SQ, 4 LHU + 8 SH, LQ) instead of `PEXTLH rd, $zero, rs` | fixed (98a841aff50e): the R5900 shuffle lowering takes the zeros of a splat from any element and tries the commuted mask: PEXTLB + PEXTLH with $zero |
| 19 | VU (HLSL) | slow code | `docs/llvm-issues/mscei-not-pipelined.hlsl`: mscei with the screen scale and offset in the MVP (two upper ops fewer) is not software pipelined: 32 cycles a vertex instead of 19 (mscei: 21) | fixed (b542386063ac): the loop was pipelined (II 20), by a kernel unrolled 5 times (100 cycles a 5 vertices); 32 cycles was the loop for the remaining vertices. Two causes: an insertion of x into w (MR32.w, a lower instruction) counted as an upper one (VUBalancePipes makes it an INSERT_ROT in every block: II 19), and modulo variable expansion unrolls for the output pointer of the last stage, not just for the copies (the copies are now kept when they leave a quarter of the lower slots free). Now 19 cycles, no unrolling; mscei 21 (24 → 21 words), mtex 48.4 → 44, tf1 25 → 23, floor 21 → 20; water 95 → 96 |
| 20 | VU (HLSL) | cannot select | `docs/llvm-issues/mscei-adc-lane.hlsl`: an output's int lane from an input's lane (`o.xyz2.w = v.nrm.w`): `Cannot select: v4f32 = bitcast (v8i16 BUILD_VECTOR ...)` | fixed (b542386063ac): an integer shuffle whose fields stay in place is a MERGE (MOVE<mask>, exact): `move.w` from the input, 17 cycles |
| 21 | VU (HLSL) | missing feature | fields of a constant in an *integer* input (`int3 uv : TEXCOORD_V2_16` with z = 4096): only float inputs of 32-bit formats get fields of 1.0. mscei's GIF_ST needs an ADDQ for its q without it, or 8-byte texture coordinates | fixed (b542386063ac): `_F<n>` after the format of an integer input (`int3 uv : TEXCOORD_V2_16_F4096`): the VIF sets the other fields from ROW (STMASK 01), the header's `ROW` gives the values and `mx_shader_e_context` sends them (STROW). Also fixed on the way: an int3 widened to int4 crashed type legalization (v6i32). mscei/msceic/mskin use it: mscei_ix 21 → 20, msceic 22 → 21, mskin 88 → 86 cycles a vertex (issue 21's shader: 18); mtex stays on int2 (48 instead of 44 with it: the loop's registers after allocation) |
| 22 | VU (HLSL) | slow code | the strip programs' input and output buffers are apart (MSCEI_MAX_N 81): ~100 cycles of each batch are prologue, epilogue and the two vertices repeated from the batch before (~6 % of mscei at 81 vertices) | dropped (2026-10-26, agreed with the user): with the outputs over the inputs, VIF1 unpacks batch k + 2 into the buffer whose GIF packet (batch k) the GIF may still read: TOPS swaps when batch k + 1 starts, not when the GIF is done (the same as the init MSCAL found on the PS2, `docs/vu1.md`). Safe forms cost more than the ~6 %: a FLUSH before each batch (no overlap of UNPACK and VU1), or one shared packet with an XGKICK wait at each batch start (the indexed layout; 4-6 % for the wait alone at the init) |

Changes after the 22:00 build (feature tree, uncommitted): the narrowed ILW of mskin's joint
indices kept no alias scopes (VUNarrowInt), which made a false 110-cycle recurrence; joint row
addresses share `ctx + 4j` with the row in the LQ offset (VULowerMemory), which frees VI
registers; field moves go to the lower pipe where it has room (VUBalancePipes before the
pipeliner, VULoopScheduler after it); a compute program's uniforms get alias scopes against
its items; VULoopAliasScopes gives a loop's loads and stores through the same base alias
scopes when their bytes over all iterations are disjoint (with c (n /u c) <= n for the trip
count); the pipeliner skips register pressure evaluations that cannot change its choice and
limits the others for large loops; an unsigned conversion's mask of the negative fields is one
MUL by -2^127 (vskin 209 → 204 words a group of four vertices). Sizes: mskin 150 → 320 words (pipelined), dust -60, fbm -4,
water +2. On the PS2 (23:25): tests/ps2 ishader (all _ix and mskin/mscei strip), shader,
shader2, dust, noise and floor pass.

## 1. VU: scalar `lshr`, `ashr` and `udiv` on i16 cannot be selected; with a `mul` llc crashes

VI has no shift instructions. `shl` and `mul` by a constant are fine. `lshr`, `ashr` and
`udiv` by a power of two fail:

| IR (i16 from `llvm.vu.ilw`, result to `llvm.vu.isw`) | llc -mtriple=vu -mcpu=vu0 -O3 |
|---|---|
| `lshr i16 %n, 2` | `LLVM ERROR: Cannot select: i16 = BUILD_VECTOR` |
| `ashr i16 %n, 1` | the same |
| `udiv i16 %n, 4` | the same |
| `shl i16 %n, 4`, `mul i16 %n, 48`, `mul i16 %n, 3`, `and i16 %n, 3` | ok |

`lshr` followed by `mul` crashes instead. Release build, SIGSEGV in DAGCombiner on the
malformed node; where depends on the input:
- `vskin.hlsl` (whole program): `isConstOrConstSplat` ← `TargetLowering::SimplifyDemandedBits`
  ← `DAGCombiner::visitADDLike`
- `vu-lshr-mul-crash.ll` (reduced): `isConstOrConstSplatFP` ← `SelectionDAG::foldConstantFPMath`
  ← `SelectionDAG::FoldConstantArithmetic` ← `DAGCombiner::visitADDLike`

The assertions build of 2026-10-24: `LegalizeDAG.cpp:4541 ExpandNode: Assertion
'VT.isVector() && "Unable to legalize non-vector shift"'`. Reduced by llvm-reduce:
`docs/llvm-issues/vu-lshr-mul-crash.ll`

Cause: `VUISelLowering.cpp` sets i16 `SRA` and `SRL` to Expand (the "16-bit integers" list).
`ExpandNode` can only expand a shift by scalarizing a vector: for a scalar type the assertion
fires, and without assertions it builds `getBuildVector(i16, ...)`, the `i16 = BUILD_VECTOR`
that either cannot be selected or crashes a later combine. `udiv` by a power of two is
combined into `srl` first, so it is the same bug. Expand is not an option for scalar shifts;
they need a Custom lowering.

```llvm
define void @vskin() {
vu.entry:
  %n = tail call i16 @llvm.vu.ilw(i16 0, i32 0)
  %0 = lshr i16 %n, 1
  %.idx = mul i16 %0, 48
  %1 = getelementptr i8, ptr addrspace(4) inttoptr (i16 1216 to ptr addrspace(4)), i16 %.idx
  %2 = addrspacecast ptr addrspace(4) %1 to ptr addrspace(11)
  store <4 x float> zeroinitializer, ptr addrspace(11) %2, align 4
  ret void
}
```

```
llc -mtriple=vu -mcpu=vu0 -O3 -filetype=obj docs/llvm-issues/vu-lshr-mul-crash.ll -o /dev/null
```

From HLSL: `docs/llvm-issues/vskin.hlsl` (`uint m = n / 4; ... nrm = n + 2 + m / 4 * 3`), the
whole program, through the Makefile's steps with `-vu-cpu=vu0`.

Wanted: a lowering for these (there are no VI shifts; through VF: ITOF0, MUL by 2^-k, FTOI0;
or a loop), or at least a diagnostic instead of a crash. `uint` `/` and `>>` by constants
are ordinary HLSL.

## 2. VU: what the VU0 skinning program needs next (not yet checked, behind 1)

`docs/llvm-issues/vskin.hlsl` is the program the demo wants. Once it compiles, these are
the things to look at, in its loop over groups of four vertices:

- `float3(asint(x[b + 1].xyz))` and `float4(asuint(x[b + 3]))`: ints that VIF0 unpacked into
  memory lanes (V4-8 / V4-8U) should be one LQ + ITOF0 in VF, not lane by lane in VI.
- `asfloat(int4(u))` stored to `x[...]`: FTOI0 of values up to 2^24 must keep 32 bits (the
  docs say `int` is 16 bits in VI; here it never needs to leave VF).
- `pal[j.x]` with `j = asuint(x[b + 2])`: ILW from a memory lane, then LQ at it (as mskin's
  joint palette), 12 loads a vertex.
- Lane packing of the output: `float4(p[0], p[1].x)`, `float4(p[1].yz, p[2].xy)`,
  `float4(p[2].z, p[3])` should be MR32 / masked moves in the lower pipe (8 per four vertices).
- `u[k] = r.x + r.y * 256 + r.z * 65536` into lane k: ADDA/MADDA/MADD with dest mask k.
- Stores past the item count (`x[n + ...]`): the output follows the inputs in data memory.
- Scheduling: per vertex about 35 FMAC ops (12 for the blend in three ACC chains, 4
  transform, 3 normal, 3 length, round, 3 pack) and about 25 lower ops, one RSQRT. A
  hand schedule would be about 35-40 cycles a vertex (FMAC bound); the loop body is four
  vertices, so the RSQRTs and ACC chains of different vertices can overlap.

## 3. `-mvu0`: a scalar float into a VU0 vector goes through the stack

`docs/llvm-issues/mvu0-vectors.c`, built with
`clang --target=mips64el -mcpu=r5900 -mabi=n32 -O2 -mvu0 -ffinite-math-only -S`:

```c
f4 scal(f4 m, float w) { return m * w; }   /* addiu swc1 lqc2 vmulx jr addiu */
```

The broadcast only needs `w` in field x. `MipsSEISelLowering.cpp` sets every v4f32 opcode
to Expand and makes only BUILD_VECTOR custom (`lowerVU0BUILD_VECTOR`, GPR + QMTC2);
SCALAR_TO_VECTOR stays Expand, which is the stack. Wanted: SCALAR_TO_VECTOR v4f32
custom, `mfc1` + `qmtc2` for an FPR value, `lw` + `qmtc2` for a loaded one
(`bitcastToI32` already turns the `lwc1` into `lw`). A union / `__builtin_bit_cast` to an
int does not avoid it: the optimizer folds it back into the float splat.

## 4. R5900 MMI: a `v4i32` built from four GPRs goes through the stack

```c
i4 v = {(int)(b & 255), (int)(b >> 8 & 255), (int)(b >> 16 & 255), (int)(b >> 24)};
return m * __builtin_convertvector(v, f4);  /* ... sw sw sw sw lq qmtc2 vitof0 vmul */
```

This is not VU0 code: a v4i32 BUILD_VECTOR never reaches `lowerVU0BUILD_VECTOR` (v4f32 only).
`lowerBUILD_VECTOR` sends it to `lowerR5900BUILD_VECTOR`, which handles splats and constants
and expands everything else, i.e. stores and reloads it. Plain `-mcpu=r5900` without `-mvu0`
gives the same `sw sw sw sw lq` for `return v;`. `lowerVU0BUILD_VECTOR` already builds a
non-splat v4f32 with PEXTLW + PCPYLD. Wanted: the same PEXTLW / PCPYLD path for a non-splat
v4i32 in `lowerR5900BUILD_VECTOR`.

## 5. R5900 MMI: byte vectors to int vectors go byte by byte

```c
f4 bytes_u(f4 m, const u8x4 *p) { return m * __builtin_convertvector(*p, f4); }
    /* lbu sw lbu sw lbu sw lbu sw lq qmtc2 vitof0 */
f4 bytes_s(f4 m, const s8x4 *p) { ... }   /* the same with lb */
void tobytes(s8x4 *o, f4 r) { *o = __builtin_convertvector(__builtin_convertvector(r, i4), s8x4); }
    /* vftoi0 qmfc2 sb pcpyud dsrl sb dsrl sb sb */
```

The `u8x4`/`s8x4` → `v4i32` step is the slow part, and it is not VU0 code: without `-mvu0`,
`__builtin_convertvector(*p, i4)` is the same `lbu sw` × 4 + `lq`. Only the VITOF0 after it is
VU0. `tobytes` does not use the stack: it is one `qmfc2`, then shifts and an `sb` for each byte.

Wanted: `lw` + PEXTLB / PEXTLH with a zero register (unsigned), then PSLLW 24 + PSRAW 24
(signed), + `qmtc2`; the other way PPACH / PPACB + `sw`. Mesh data is mostly bytes
(normals, joints, weights, colours), so this is in every skinning or lighting loop.

## 6. `-mvu0`: storing lanes of a VU0 vector goes through the FPU

```c
void lanes(float *o, f4 r) { o[0] = r[0], o[1] = r[1], o[2] = r[2]; }
    /* qmfc2 sll mtc1 swc1 dsrl32 pcpyud mtc1 mtc1 swc1 swc1 */
```

Wanted: `qmfc2` then `sw` of the GPR (and `dsrl32` / `pcpyud` for the other lanes), with no
`mtc1`. (`sd` for x and y together when the address is 8-aligned.)

Measured effect of 3-6: the EE skinning loop in demos/model/model.c (VU0 macro mode,
written to avoid them where it could) took 200-230 EE cycles a vertex on the PS2, about 30
ms a frame for Queen Marika (38k vertices); the chain of dependent COP2 ops through the
stack round trips is most of it.

## 7. VU: mskin's loop is not pipelined (112 cycles a vertex)

```
make .../hlsl/mskin.vu.ll
llc -mtriple=vu -mcpu=vu1 -O3 mskin.vu.ll -o mskin.vs
```

The inner loop is 112 instructions for one vertex (`iaddi vi3, vi3, -1`) with four WAITQ
(one DIV for the perspective, three RSQRT: the normal and the two in `vu_spec`). mtex,
the same program without the joint blend, gets a loop of 228 words for 5 vertices (46 a
vertex). With the joint blend (16 LQ at run-time ILW indices, `joint[j.x]`) the loop is no
longer software pipelined, so the Q unit and the load latencies serialize. On the PS2
mskin draws 19.3 fps indexed / 11.6 strips for Queen Marika (36.6k triangles), VU1 82-87 %
busy.

## 8. VU: the indexed mskin does not fit 31 VF registers with V3-16 positions

To halve the size of the bind pose positions (RPOS) in the model file, mskin reads them as
V3-16: `int4 pos : POSITION_V3_16` and `float4 pos = vu::itof15(v.pos)` (1 / 32768 of the
model's size; the size and offset go into the joint matrices). `docs/llvm-issues/mskin_v316.hlsl`
is demos/model/mskin.hlsl with that change (the input and one line).

- The strip program builds: `VU_MSKIN_ALIGN 8`, `IN_POSITION v3_16, 6 bytes`, one ITOF15.
- The indexed one (`opt -vu-indexed=32`, the Makefile's `mskin_ix`) fails in llc:
  `error: in function mskin void (): VU: the spills need 1 quadwords at the top of data
  memory, more than the 0 reserved ("vu-spill-qw")`. The V3-32 version builds. The only
  difference is the ITOF15 of the loaded position.

`-vu-spill-qw=1` would build it, with a spill store and load in the vertex loop. Wanted: an
indexed program that keeps the extra temporary in a register (the converted position can
replace the loaded one; nothing else needs the int lanes).

## 9. VU: `float3(int3) * 2^-15` is not one ITOF15

With `int3 pos : POSITION_V3_16` and `float3(v.pos) * (1.0 / 32768.0)` the strip mskin gets an
ITOF0 and a MUL by a constant (and a register for the constant: with it, mskin_ix spills too).
`vu::itof15(int4)`, which is the same expression on 4 lanes, folds into one ITOF15. Wanted: the
fold for 3-lane (and 2-lane) vectors too.

## 10-12. VU: the VU0 skinning program (vskin) compiles, slowly, to 46 cycles a vertex

`docs/llvm-issues/vskin.hlsl` (now with `asint` for the weights: `asuint` cost ~20 FMAC ops a
group to correct values of 2^31 and up, which bytes never reach; the source's fault):

```
clang -cc1 -triple spirv-unknown-vulkan1.3-library -x hlsl -emit-llvm -O2 -finclude-default-header \
  -Iinclude -Wno-deprecated-builtins docs/llvm-issues/vskin.hlsl -o vskin.spv.ll
opt -mtriple=vu -O2 -vu-cpu=vu0 vskin.spv.ll -S -o vskin.vu.ll
llc -mtriple=vu -mcpu=vu0 -O3 vskin.vu.ll -o vskin.vs       # 122 s
```

Layout as wanted: PAL 1-72, TAG 73-75, items from 76 (32 vertices: up to 239). Right: the
inputs are one LQ + ITOF0 each; the palette ILW + LQ; the stores past the item count; the four
RSQRT results are gathered into one vector and scaled by 127 with one MUL; the rounding is
MULA + MADD + ADD. The loop (one group of four vertices) is 185 words: 152 upper ops, 100
lower, 2 WAITQ.

- 10: llc needs 122 s (clang + opt under a second). Likely the scheduler or the register
  allocator on the long loop body (four vertices, ~28 ACC chains).
- 11: 31 of the upper ops are lane inserts `ADDbc.f d, vf0, s`: the translation out of the
  columns' w (`float3(c0.w, c1.w, c2.w)`), the V3-32 packing of the positions
  (`float4(p[0], p[1].x)` ...) and a 4 × 3 transpose before the normal packing. The upper pipe
  is the limit (152 vs 100 lower), so they should be MR32 / MOVE in the lower pipe; the
  transpose could go if `u[k] = ...` were ADDA / MADDA / MADD with dest mask k.
- 12: without 11, about 121 upper ops a group, 30 a vertex; the loop is one group an iteration
  with its RSQRTs at the end (WAITQ), not software pipelined across groups.

## 13. `-mvu0`: the MachinePipeliner gets an EE skinning loop wrong

`demos/model/model.c` `skin_vertices` (three branch-free loops: vertices of one joint, of
two, of four; `#pragma clang loop unroll_count(2)`), inlined into `skin_run` twice, with and
without normals. The loop of one joint without normals (one LDL/LDR + PEXTLH + QMTC2 + VITOF0,
four LQC2 of the joint's columns, VMULAX/VMADDAY/VMADDAZ/VMADDW, one SQC2) is software
pipelined (`bgtz` on the trip count minus 6, a prologue and an epilogue); the other loops are
not. In PCSX2, over the level-0 clusters of Queen Marika (99,673 vertices), the
positions of that loop differ from the same loop with normals for 2,084 vertices, all in
clusters whose 32 vertices all have one joint (long trip counts), e.g.
`0098a570 00000000 00000000` instead of `bf20ba32 3f8bc291 bd3a7772`. With
`-mllvm -enable-pipeliner=false` all 99,673 are equal, and the strips and indexed pictures of
MFLAT (the only program that skins without normals) are the same again.

`docs/llvm-issues/mvu0-pipeliner-skin_run.ll`: `skin_run` of model.c (`llvm-extract`), the
pipelined loop is the one with the `bgtz`:

```
llc -O2 -mtriple=mips64el -mcpu=r5900 -target-abi n32 -mattr=+vu0 mvu0-pipeliner-skin_run.ll -o -
```

A smaller program with the same loop body and synthetic data (also pipelined) gave the right
results, so the trigger is in the schedule of this one; not reduced further. The register
reuse across stages is where I would look: VF registers are reloaded as different columns in
different stages (e.g. vf7 is column 3 of one stage and column 1 of the next), and the ACC
chains of different stages interleave around the single ACC.

## 14. clang drops `pipeline(disable)` next to `unroll_count`

`docs/llvm-issues/pipeline-disable-unroll.c`: with `unroll_count(2) pipeline(disable)` (one
pragma, two pragmas, or through `_Pragma`) the loop gets only `llvm.loop.unroll.count 2`; alone,
`pipeline(disable)` gives `llvm.loop.pipeline.disable`. In `CGLoopInfo.cpp`,
`createPartialUnrollMetadata` puts the pipelining attributes in a `llvm.loop.unroll.followup_all`
only if `FollowupHasTransforms`, and `createPipeliningMetadata` sets `HasUserTransforms` only
when pipelining is enabled (an initiation interval), not for `pipeline.disable`. So 13 cannot
be worked around per loop.

## 15. `-mvu0`: a VU0 lane's reciprocal square root goes through the FPU

```c
f32x4 t2 = t * t;
t = t * (127.0f / __builtin_sqrtf(t2[0] + t2[1] + t2[2] + 1e-30f));
```

is VMUL, VADDY, VADDZ, then QMFC2, MTC1, ADD.S, SQRT.S, DIV.S, MFC1, QMTC2, VMULX: two FPU
operations that do not pipeline and six moves on the chain. VU0 has VRSQRT Q, VF.x, VF.y (and
VSQRT); with VWAITQ and VMULQ the scale would stay in VU0, the way `VDIV_V` / `VMULQ_V` already
do division. In the demo the normals' renormalization is ~3 of the 16 ms of EE skinning a frame
(PCSX2, measured by leaving it out). `rsqrt.s` (afn) would at least remove the DIV.S.

## 16-18. VU0 micro mode fed through VF registers (probe/p15_vu0skin.c)

The probe skins with VU0 three ways: macro mode (model.c's loops), micro mode fed through VF
registers (two register sets: the EE fills one with QMTC2 while VU0 runs the program of the
other, VCALLMS starts the next program when the running one ends, SQC2 / QMFC2 read the
results), and micro mode fed by VIF0 DMA. All give the same results as macro mode (PCSX2,
2026-10-26). The EE side is written in assembly for now, because of these:

- 16: `docs/llvm-issues/mvu0-cop2-asm.c`. The assembler takes QMTC2, QMFC2, LQC2, SQC2 and
  VNOP with `-mvu0`, but not VCALLMS, VCALLMSR, or CFC2 / CTC2 with a VU0 control register
  (vi29 VPU-STAT, vi27 CMSAR0, vi28 FBRST) or a VI register. maxsdk uses `.word`.
- 17: the same file. The inputs and results of the two sets (vf1..vf12) are live between the
  asm statements. A clobber covers only its own statement and there is no `-ffixed-vfN`, so
  any `-mvu0` code the compiler puts in between may overwrite them, and its macro
  instructions would also wait for the running micro program (`docs/vu0.md`). Today the
  register-fed loop must not contain float vector code.
- 18: `docs/llvm-issues/mmi-zext-halfword.c`. The joint slots and weights need four bytes
  zero-extended to words. PEXTLB with `$zero` is found for the bytes; the halfwords then go
  through the stack (SQ, LHU / SH, LQ) instead of `PEXTLH rd, $zero, rs`. The sign-extended
  form with the zeros below (model.c's normals) is fine.

## 19-22. The strip mscei against ps2gl's SCEI (VCL)

Found while looking at how the model demo's strips could be drawn faster at SCEI quality
(2026-10-26, the `toolchain/build/llvm` build, clang af4807c366f1). Cycles a vertex of the
steady-state loop (each loop is full: an upper op in every cycle, the delay slot included):

| program | cycles | inputs | does |
|---|---|---|---|
| ps2gl `libs/ps2gl/vu1/scei.vcl` (VCL, `scei_vcl.vsm` MAIN_LOOP) | 19 | position, normal, stq: floats (q = 1) | transform, 3 lights, clipping (CLIPW), strip ADC from the input |
| `demos/model/msceic.hlsl` | 22 | position V3-32, normal V4-8, uv V2-16 | the same, ADC from clipping only |
| `demos/model/mscei.hlsl` | 21 | the same | no clipping |
| mscei, scale and offset in the MVP (19) | 32, 19 forced | the same | |
| mscei, + `float3 st : TEXCOORD_V2_32` (VIF fills q = 1.0) | 17 | uv 8 bytes | |
| mscei, + the strip restart flag in the normal's w (`vu::adc`) | 17 | the same | ADC from the input (lower pipe: ILW.w, IAND, IADDIU, MFIR.w) |

The compiler's schedule is as tight as VCL's: with the same inputs the loop has the same upper
ops. The three cycles msceic has over VCL are the conversions of the packed inputs (ITOF0 of
the normal, ITOF12 of the texture coordinates, ADDQ for GIF_ST's q). Wanted, so that mscei
gets ps2gl's speed with the packed (smaller) inputs:

- 19: `docs/llvm-issues/mscei-not-pipelined.hlsl` is mscei without `t * scl + prm` (the EE
  puts the scale and offset in the MVP: one MULA and one MADD less). The pipeliner tries
  II 19 and 20, finds no schedule and gives up: the loop is not pipelined, 32 cycles.
  `llc --pipeliner-force-ii=21` pipelines it, and the loop is then 19 cycles (2 fewer than
  mscei, as expected). `--pipeliner-ii-search-range=40` and `--pipeliner-max-stages=6` do not
  help. Wanted: the search goes on to larger IIs (a loop that becomes 13 cycles slower for two
  ops less makes the shaders' speed hard to predict).
- 20: `docs/llvm-issues/mscei-adc-lane.hlsl`. `o.xyz2.w = v.nrm.w` (also
  `int4(vu::ftoi4(...).xyz, v.nrm.w)`) stops llc: `Cannot select: v4f32 = bitcast (v8i16
  BUILD_VECTOR ...)`. Expected: a MOVE.w in the lower pipe from the unpacked input (its w is the
  sign-extended byte, so -128 sets bit 15, the ADC bit). `vu::adc(uint(v.nrm.w) & 1)` works,
  with four lower ops instead of one.
- 21: a field of a constant in an integer input. With `int3 uv : TEXCOORD_V2_16` and z = 4096
  from the VIF (STMASK with ROW, or COL when no float input needs 1.0), `float4(vu::itof12(uv) *
  q, 0)` would be ITOF12 + MULq, without the ADDQ: 18 cycles with 4-byte texture coordinates
  (17 with `TEXCOORD_V2_32`, 8 bytes). Today only float inputs of 32-bit formats get fields of
  1.0 (`docs/hlsl.md`).
- 22: a strip batch has ~100 cycles outside the loop (the pipelined loop's prologue and
  epilogue, the batch header and the kick) and repeats the last two vertices of the batch
  before: at MSCEI_MAX_N 81 about 6 % of mscei's time. The input (3 QW a vertex) and output
  (3 QW) buffers are apart; with the outputs written over the inputs (the same size here, and
  each input is read before its output is stored), MAX_N would be ~160 and the share half.
  Doable in the layout (VU_IN_AT = VU_OUT_AT + 1 when ITEM_QW = OUT_QW).

Not compiler work, for the record: the demo's strips are short (they are cut inside the
32-vertex clusters: 4.9 triangles a strip for Queen Marika, 1.81 strip vertices a triangle),
and each join repeats two vertices; the ADC restart (20) instead of the repeats saves ~22 % of
the vertices.

## Cost of V3-16 positions in the indexed programs (not a compiler bug)

V3-16 makes `VU_IX_MSKIN_ALIGN` 8 (6 bytes × 8 = 3 quadwords). The batch reads up to ALIGN - 1
vertices after the cluster, so the buffer needs room for 39 instead of 35 vertices: BUF_QW 200
→ 216 and `MAX_I` 96 → 80 for mskin_ix. For Queen Marika full the clusters hardly change
(4502 instead of 4504: they are limited by 32 vertices). The over-read could go if UNPACK took
the vertex count and the DMA whole quadwords (the bytes after it are VIF NOPs if zero).

## Status of the demo work

Stopped at 1, as asked. Done so far:
- `toolchain/build/model_work/lod/marika_full.mxm`: Queen Marika Full Nude at full detail
  (103,240 triangles, 99,723 cluster vertices, 182 joints, 30 palettes, 7 animations,
  indexed only, 6.6 MB), made by `lod/full.sh`.
- The design of the VU0 path: VIF0 unpacks each cluster from the MXM streams (positions
  V3-32, normals V4-8, joints as slot × 3 from a stream the EE makes at load, weights
  V4-8U) into 4 quadwords a vertex; the palette is 24 joints × 3 quadwords (the columns,
  the translation in their w); vskin writes `[tag][positions V3-32][tag][normals][tag]`
  after the inputs, and one toSPR puts it into the frame's scratchpad half as finished VIF1
  packets (fromSPR → MFIFO → VIF1). The normals go as n + 128 and VIF1 unpacks them
  unsigned with STMOD offset and ROW -128: exactly the signed V4-8 values, so the VU1
  programs do not change. 72 + 128 + 35 + a few quadwords of VU0 memory.

Smaller model files: done after 8 and 9 were fixed (MXM3: no rest pose streams for skinned
models, RPOS as V3-16, clusters and strip runs from multiples of 8). Queen Marika full: 6.63
→ 4.45 MB. ishader passes (mskin_ix V3-16 bit for bit, mskin against the EE); the model demo
passes its picture checks (Fox: all six programs, also with -static); EE skinning (MTEX) and
mskin give the same picture of Queen Marika full (1 pixel of coverage differs); every vertex
is within half a step (13.1 µm) of its float32 position.

The VU0 skinning program is not built into the demo yet: 10-12 (46 instead of ~31-35 cycles
a vertex).
