// procgen terrain, near (quality level 2 with fog): lit, with the detail
// texture (V2-16 coordinates in 4.12), perspective correct.
#include "vu.hlsli"

cbuffer Uniforms {
    float4x4 mvp;
    float4 fogc;
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
    int4 xyzf2 : GIF_XYZF2;
};

[shader("vertex")]
Out tf2(In v) {
    float4 p = mul(mvp, float4(v.pos, 1));
    float q = rcp(p.w);
    float d = max(dot(float3(v.nrm.xyz), light.xyz), 0.0f);
    float fog = clamp(p.w * fogc.x + fogc.y, 0.0f, 255.0f);
    Out o;
    o.st = float4(vu::itof12(v.uv) * q, q, 0);
    o.rgbaq = int4(min(float4(v.col) * (amb + lcol * d), (float4)255)); // RGBAQ keeps 8 bits
    o.xyzf2 = vu::ftoi4(float4((p * q).xyz, fog));
    return o;
}
