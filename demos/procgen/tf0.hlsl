// procgen terrain, far (quality level 0 with fog): position V3-32, colour
// V4-8 (0..255) to RGBAQ, fog by depth.
#include "vu.hlsli"

cbuffer Uniforms {
    float4x4 mvp;
    float4 fogc;    // fog = clamp(w * x + y, 0, 255): 255 none, 0 all fog
};

struct In {
    float3 pos : POSITION;
    uint4 col : COLOR_V4_8U;
};

struct Out {
    uint4 rgbaq : GIF_RGBAQ;
    int4 xyzf2 : GIF_XYZF2;
};

[shader("vertex")]
Out tf0(In v) {
    float4 p = mul(mvp, float4(v.pos, 1));
    float fog = clamp(p.w * fogc.x + fogc.y, 0.0f, 255.0f);
    Out o;
    o.rgbaq = v.col;
    o.xyzf2 = vu::ftoi4(float4((p / p.w).xyz, fog));
    return o;
}
