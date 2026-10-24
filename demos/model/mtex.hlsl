// Model demo: lit as mlit and textured, perspective correct; the texture
// coordinates are V2-16 in 4.12 fixed point. The colour modulates the
// texture: 128 is 1.0.
#include "vu.hlsli"

cbuffer Uniforms {
    float4x4 mvp;
    float4 light;   // towards the light in model space, divided by 127; w 0
    float4 lcol;    // light colour, 128 = 1.0; w 0
    float4 amb;     // ambient colour; w the alpha
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
Out mtex(In v) {
    float4 p = mul(mvp, float4(v.pos, 1));
    float q = rcp(p.w);
    float d = max(dot(float3(v.nrm.xyz), light.xyz), 0.0f);
    Out o;
    o.st = float4(vu::itof12(v.uv) * q, q, 0);
    o.rgbaq = int4(min(amb + lcol * d, (float4)255));
    o.xyz2 = vu::ftoi4(p * q);
    return o;
}
