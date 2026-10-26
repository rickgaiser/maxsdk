// LLVM issue 20: one int lane of an output from an input's lane (here the strip
// restart flag in the normal's w byte into XYZ2's w, for its ADC bit) cannot be
// selected: "Cannot select: v4f32 = bitcast (v8i16 BUILD_VECTOR ...)" in llc. Also
// as int4(vu::ftoi4(...).xyz, v.nrm.w). Expected: a MOVE.w in the lower pipe.
// vu::adc(uint(v.nrm.w) & 1) works (ILW.w, IAND, IADDIU, MFIR.w). Build as
// mscei-not-pipelined.hlsl.
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
    float3 st : TEXCOORD_V2_32;
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
    o.st = float4(v.st * q, 0);
    o.rgbaq = vu::rgba8(float4(l, amb.w));
    o.xyz2 = vu::ftoi4(float4(t, 0));
    o.xyz2.w = v.nrm.w;
    return o;
}
