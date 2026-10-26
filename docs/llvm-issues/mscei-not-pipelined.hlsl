// LLVM issue 19: demos/model/mscei.hlsl with the screen scale and offset in the MVP
// (one MADD and one MULA less than mscei): the item loop is not software pipelined,
// 32 cycles a vertex instead of 19 (mscei: 21). The pipeliner gives up after II 19 and
// 20; llc --pipeliner-force-ii=21 gives a loop of 19 cycles. Build as the Makefile:
//   clang -cc1 -triple spirv-unknown-vulkan1.3-library -x hlsl -emit-llvm -O2 \
//     -finclude-default-header -I include -Wno-deprecated-builtins <this> -o x.spv.ll
//   opt -mtriple=vu -O2 -vu-cpu=vu1 x.spv.ll -S -o x.vu.ll
//   llc -mtriple=vu -mcpu=vu1 -O3 x.vu.ll -o x.s
#include "vu.hlsli"

cbuffer Uniforms {
    float4x4 mvp;
    float4 a[3];    // a[0] x + a[1] y + a[2] z = the three cosines (n of length 127)
    float4 e[3];    // light colours, 128 = 1.0
    float4 amb;     // w: alpha (the highlight of TFX HIGHLIGHT2: 0)
    float4 prm;     // xyz: the screen offset, w: the guard band
    float4 scl;     // xyz: the scale before the offset
};

struct In {
    float3 pos : POSITION;
    int4 nrm : NORMAL_V4_8;
    int2 uv : TEXCOORD_V2_16;
};

struct Out {
    float4 st : GIF_ST;
    int4 rgbaq : GIF_RGBAQ;
    int4 xyz2 : GIF_XYZ2;
};

[shader("vertex")]
Out mscei(In v) {
    float4 p = mul(mvp, float4(v.pos, 1));
    float q = rcp(p.w);
    float3 t = p.xyz * q;
    float3 n = float3(v.nrm.xyz);
    float3 c = max(a[0].xyz * n.x + a[1].xyz * n.y + a[2].xyz * n.z, (float3)0);
    float3 l = min(e[0].xyz * c.x + e[1].xyz * c.y + e[2].xyz * c.z + amb.xyz, (float3)255);
    Out o;
    o.st = float4(vu::itof12(v.uv) * q, q, 0);
    o.rgbaq = vu::rgba8(float4(l, amb.w));
    o.xyz2 = vu::ftoi4(float4(t, 0));
    return o;
}
