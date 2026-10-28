// vumax S2 in HLSL (the twin of vx_gour): S1 with a colour per vertex.
#include "vu.hlsli"

cbuffer Uniforms {
    float4x4 mvp;
};

struct In {
    float3 pos : POSITION;
    uint4 col : COLOR_V4_8U;
};

struct Out {
    int4 rgbaq : GIF_RGBAQ;
    int4 xyz2 : GIF_XYZ2;
};

[shader("vertex")]
Out vh_gour(In v) {
    float4 p = mul(mvp, float4(v.pos, 1));
    Out o;
    o.rgbaq = int4(v.col);
    o.xyz2 = vu::ftoi4(p * rcp(p.w));
    return o;
}
