// Quality level 0 (PLAN_PROCGEN): far terrain. The position (V3-32) is
// transformed and projected; the colour (V4-8, 0..255, e.g. from the height)
// goes to RGBAQ unchanged. Gouraud, untextured, no clipping: the EE culls
// chunks that are not fully inside the guard band.
#include "vu.hlsli"

cbuffer Uniforms {
    float4x4 mvp;   // P = mvp * (x, y, z, 1); P / P.w in GS units
};

struct In {
    float3 pos : POSITION;
    uint4 col : COLOR_V4_8U;
};

struct Out {
    uint4 rgbaq : GIF_RGBAQ;
    int4 xyz2 : GIF_XYZ2;
};

[shader("vertex")]
Out lvl0(In v) {
    float4 p = mul(mvp, float4(v.pos, 1));
    Out o;
    o.rgbaq = v.col;
    o.xyz2 = vu::ftoi4(p / p.w);
    return o;
}
