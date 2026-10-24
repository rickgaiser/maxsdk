// Quality level 2 (PLAN_PROCGEN): level 1 textured, perspective correct. The
// texture coordinates are V2-16 in 4.12 fixed point.
#include "vu.hlsli"

cbuffer Uniforms {
    float4x4 mvp;
    float4 light;
    float4 lcol;
    float4 amb;
};

struct In {
    float3 pos : POSITION;
    int4 nrm : NORMAL_V4_8;
    uint4 col : COLOR_V4_8U;
    int2 uv : TEXCOORD_V2_16;
};

struct Out {
    float4 st : GIF_ST;
    int4 rgbaq : GIF_RGBAQ;
    int4 xyz2 : GIF_XYZ2;
};

[shader("vertex")]
Out lvl2(In v) {
    float4 p = mul(mvp, float4(v.pos, 1));
    float q = rcp(p.w);
    float d = max(dot(float3(v.nrm.xyz), light.xyz), 0.0f);
    Out o;
    o.st = float4(vu::itof12(v.uv) * q, q, 0);
    o.rgbaq = int4(min(float4(v.col) * (amb + lcol * d), (float4)255)); // RGBAQ keeps 8 bits
    o.xyz2 = vu::ftoi4(p * q);
    return o;
}
