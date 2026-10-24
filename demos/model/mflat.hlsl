// Model demo, the cheapest shader: the position (V3-32) transformed and
// projected, one colour for the whole draw.
#include "vu.hlsli"

cbuffer Uniforms {
    float4x4 mvp;   // P = mvp * (x, y, z, 1); P / P.w in GS units, Z = P.z / P.w
    float4 col;     // RGBA, 0..255
};

struct In {
    float3 pos : POSITION;
};

struct Out {
    int4 rgbaq : GIF_RGBAQ;
    int4 xyz2 : GIF_XYZ2;
};

[shader("vertex")]
Out mflat(In v) {
    float4 p = mul(mvp, float4(v.pos, 1));
    Out o;
    o.rgbaq = int4(col);
    o.xyz2 = vu::ftoi4(p / p.w);
    return o;
}
