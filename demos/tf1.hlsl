// procgen terrain, middle (quality level 1 with fog): one directional light.
#include "vu.hlsli"

cbuffer Uniforms {
    float4x4 mvp;
    float4 fogc;
    float4 light;   // towards the light, divided by 127; w 0
    float4 lcol;    // light colour (1 = the base colour); w 0
    float4 amb;     // ambient; w 1 keeps the alpha
};

struct In {
    float3 pos : POSITION;
    int4 nrm : NORMAL_V4_8;
    uint4 col : COLOR_V4_8U;
};

struct Out {
    int4 rgbaq : GIF_RGBAQ;
    int4 xyzf2 : GIF_XYZF2;
};

[shader("vertex")]
Out tf1(In v) {
    float4 p = mul(mvp, float4(v.pos, 1));
    float d = max(dot(float3(v.nrm.xyz), light.xyz), 0.0f);
    float fog = clamp(p.w * fogc.x + fogc.y, 0.0f, 255.0f);
    Out o;
    o.rgbaq = int4(min(float4(v.col) * (amb + lcol * d), (float4)255)); // RGBAQ keeps 8 bits
    o.xyzf2 = vu::ftoi4(float4((p / p.w).xyz, fog));
    return o;
}
