// Model demo: Gouraud with one directional light and ambient light. The
// normal is V4-8 (signed, length 127).
#include "vu.hlsli"

cbuffer Uniforms {
    float4x4 mvp;
    float4 light;   // towards the light in model space, divided by 127; w 0
    float4 lcol;    // light colour times the material colour (0..255); w 0
    float4 amb;     // ambient colour times the material colour; w the alpha
};

struct In {
    float3 pos : POSITION;
    int4 nrm : NORMAL_V4_8;
};

struct Out {
    int4 rgbaq : GIF_RGBAQ;
    int4 xyz2 : GIF_XYZ2;
};

[shader("vertex")]
Out mlit(In v) {
    float4 p = mul(mvp, float4(v.pos, 1));
    float d = max(dot(float3(v.nrm.xyz), light.xyz), 0.0f);
    Out o;
    o.rgbaq = int4(min(amb + lcol * d, (float4)255)); // RGBAQ keeps 8 bits
    o.xyz2 = vu::ftoi4(p / p.w);
    return o;
}
