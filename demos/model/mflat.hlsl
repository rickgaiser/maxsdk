// Model demo, the cheapest shader: the position (V3-32) transformed and
// projected, one colour for the whole draw.
#include "vu.hlsli"

cbuffer Uniforms {
    float4x4 mvp;   // P = mvp * (x, y, z, 1); P / P.w in GS units, Z = P.z / P.w
    float4 col;     // RGBA, 0..255
};

struct In {
    // w: 2056.0 where a strip restarts, else 0: its bits (0x45008000) and
    // its FTOI4 (0x8080) have bit 15, XYZ2's ADC (no triangle ends at
    // this vertex); V4-32 in strips, 1.0 from the VIF when the demo
    // sends V3-32 (indexed)
    float4 pos : POSITION_V3_32;
};

struct Out {
    int4 rgbaq : GIF_RGBAQ;
    int4 xyz2 : GIF_XYZ2;
};

[shader("vertex")]
Out mflat(In v) {
    float4 p = mul(mvp, float4(v.pos.xyz, 1));
    Out o;
    o.rgbaq = int4(col);
    o.xyz2 = vu::ftoi4(p / p.w);
    o.xyz2.w = asint(v.pos.w);
    return o;
}
