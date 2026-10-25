// vumax S3b in HLSL (the twin of vx_tex16): S3 with ST as V2-16 in 4.12
// fixed point.
#include "vu.hlsli"

cbuffer Uniforms {
    float4x4 mvp;
};

struct In {
    float3 pos : POSITION;
    uint4 col : COLOR_V4_8U;
    int2 st : TEXCOORD_V2_16;
};

struct Out {
    float4 st : GIF_ST;
    int4 rgbaq : GIF_RGBAQ;
    int4 xyz2 : GIF_XYZ2;
};

[shader("vertex")]
Out vh_tex16(In v) {
    float4 p = mul(mvp, float4(v.pos, 1));
    float q = rcp(p.w);
    Out o;
    o.st = float4(vu::itof12(v.st) * q, q, 0);
    o.rgbaq = int4(v.col);
    o.xyz2 = vu::ftoi4(p * q);
    return o;
}
