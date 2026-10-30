// Tutorial 3, step 2: 3D. The position is transformed by one matrix, the
// model, the view and the projection together (mvp); the GS divides by w.
#include "vu.hlsli"

cbuffer Uniforms {
    float4x4 mvp;   // four quadwords, a column each
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
Out t3_cube(In v) {
    float4 p = mul(mvp, float4(v.pos, 1));
    Out o;
    o.rgbaq = int4(v.col);
    // x / w, y / w: the screen; z / w: the depth for the Z buffer.
    // rcp is the VU's DIV, one per vertex.
    o.xyz2 = vu::ftoi4(p * rcp(p.w));
    return o;
}
