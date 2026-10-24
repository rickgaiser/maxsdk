# HLSL for the VUs

VU programs are written in HLSL and compiled by the toolchain's clang, opt and llc (GOAL_VU_HLSL,
llvm `lib/Target/VU/VUEntryABI.md`). Each `[shader("vertex")]` or `[shader("compute")]` function
of a `.hlsl` file in `demos/` or `tests/ps2/` is a micro program; the Makefile makes `<name>.o`
(the code, symbols `<name>_vu` and `<name>_vu_end`, section `.vutext`) and `<name>.h` (the data
memory layout, `VU_<NAME>_...`). Programs for VU0 are listed in `HLSL_VU0`.

```
clang -cc1 -triple spirv-unknown-vulkan1.3-library -x hlsl -emit-llvm -O2 -finclude-default-header x.hlsl
opt -mtriple=vu -O2 -vu-cpu=vu0|vu1     (VUHLSLAdapter, VUEntryLowering)
llc -mtriple=vu -mcpu=vu0|vu1 -O3 -filetype=obj
llvm-objcopy --dump-section .vu.header=x.h
```

## Vertex programs (VU1)

```hlsl
#include "vu.hlsli"

cbuffer Uniforms {          // VU_LVL2_MVP ..., from quadword 1 of the buffer
    float4x4 mvp;           // column_major: one quadword per column
    float4 light;
};

struct In {
    float3 pos : POSITION;              // V3-32: the 32-bit format of the type
    uint4 col : COLOR_V4_8U;            // UNPACK V4-8, zero-extended
    int2 uv : TEXCOORD_V2_16;           // V2-16
};

struct Out {                            // GIF registers, in this order
    float4 st : GIF_ST;
    int4 rgbaq : GIF_RGBAQ;
    int4 xyz2 : GIF_XYZ2;
};

[shader("vertex")]
Out lvl2(In v) { ... }
```

| Need | HLSL |
|---|---|
| UNPACK format of an input | a semantic suffix: `_S_32 _V2_16 _V3_8 _V4_8U _V4_5 ...` (`U`: zero-extended); none: `float`/`float2..4` → S/V2/V3/V4-32 |
| GIF register of an output | the semantic `GIF_RGBAQ GIF_ST GIF_UV GIF_XYZ2 GIF_XYZF2 GIF_FOG GIF_XYZ3 GIF_XYZF3`; two outputs to one register: `GIF_XYZF2_A`, `GIF_XYZF2_B` |
| Skip a vertex (ADC) | bit 15 of XYZ2/XYZF2.w: add 2048 before `vu::ftoi4` (`demos/lvl3.hlsl`) |
| State from vertex to vertex (strips) | a `static` variable that the program writes; each batch starts with its initializer |
| Fixed point | `vu::ftoi4/12/15`, `vu::itof4/12/15` (FTOI/ITOF); `int4(f)`, `float4(i)` are FTOI0/ITOF0 |
| The vertex index in the batch | `uint id : SV_VertexID` |

The layout is the one `mx_frame_shader_draw` uses: `MX_VU_SHADER(NAME, name_vu, name_vu_end)`.

## Compute programs (VU0 or VU1)

```hlsl
cbuffer Params { float4 c; };          // VU_DUST_C, from quadword 1
RWStructuredBuffer<float4> x;          // the items, from VU_DUST_SIZE

[shader("compute")]
[numthreads(1, 1, 1)]
void dust(uint i : SV_DispatchThreadID) { x[i] = ...; }   // once per item
```

With `SV_DispatchThreadID` the code runs once per item of the batch. Without it, it runs once per
batch and loops itself: `x.GetDimensions(n, stride)` gives the item count (`demos/fbm.hlsl`,
`tests/ps2/nest.hlsl`). One structured buffer; an item is a whole number of quadwords. The layout
is the one `mx_vuq_batch` uses: `MX_VU_KERNEL(NAME, vu, name_vu, name_vu_end)`.

## What to know

| Topic | Rule |
|---|---|
| Integers | `int`/`uint` are 16 bits (VI registers); a 32-bit value in memory is its low half, a stored one is sign-extended |
| Floats | the VU's: no NaN, infinity or denormals; `a > b` is `(a - b) * 2^127` clamped to 0..1, so values closer than 2^-126 compare equal |
| Math | `sin cos tan exp exp2 log log2 pow atan atan2 asin acos floor ceil round trunc frac` are FMAC sequences (error ~1e-6, floor exact); `sqrt`, `rsqrt`, `rcp` and `/` use DIV/SQRT (Q) |
| Reassociation | HLSL lets the compiler reassociate float math; `mul()` keeps the order of the columns. Code that needs the exact order, like a hash of a fraction, says `#pragma clang fp reassociate(off)`, for a file (`demos/fbm.hlsl`) or a `{ }` block (`demos/dust.hlsl`, `demos/water.hlsl`) |
| Constant tables | indexed by constants only (`static const` arrays at run-time indices are not supported) |
| Spills | none by default: a program that needs more than the 31 VF registers is an error; reserve `-vu-spill-qw=N` quadwords at the top of data memory |
| Names | `clip` is an HLSL function; a uniform needs another name (`cull`) |

## Programs

| Program | Unit | What | Test |
|---|---|---|---|
| `demos/lvl0..3.hlsl` | VU1 | procgen quality levels | `tests/ps2/shader.c` (every batch size, against the EE) |
| `demos/tf0..2.hlsl`, `water.hlsl`, `tree.hlsl`, `dustdraw.hlsl` | VU1 | procgen terrain, water, trees, dust sprites | `tests/ps2/shader2.c`, `tests/ps2/dust.c` |
| `demos/fbm.hlsl` | VU0 | terrain noise | `tests/ps2/noise.c` |
| `demos/dust.hlsl` | VU0 | dust motes | `tests/ps2/dust.c` |
| `tests/ps2/floor.hlsl`, `nest.hlsl` | VU0 | floor, nested run-time loops | `tests/ps2/floor.c`, `tests/ps2/loops.c` |
