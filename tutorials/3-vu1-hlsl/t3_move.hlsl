// Tutorial 3, step 1: a vertex program. VU1 runs it once for every vertex:
// it reads the vertex's inputs (a position and a colour) and writes the GIF
// registers the GS draws it with (RGBAQ, then XYZ2). The uniforms are the
// same for all vertices of a draw.
#include "vu.hlsli"

cbuffer Uniforms {
    float4 turn;    // x: cos, y: sin of the angle; z: the size
    float4 offset;  // xy: where the shape's centre goes, in GS coordinates
};

struct In {
    float3 pos : POSITION;      // three floats a vertex (V3-32)
    uint4 col : COLOR_V4_8U;    // four bytes a vertex (V4-8, unsigned)
};

struct Out {
    int4 rgbaq : GIF_RGBAQ;
    int4 xyz2 : GIF_XYZ2;
};

[shader("vertex")]
Out t3_move(In v) {
    // turned and scaled; y halved, as a line is twice as tall as a pixel
    float2 p = float2(v.pos.x * turn.x - v.pos.y * turn.y,
                      (v.pos.x * turn.y + v.pos.y * turn.x) * 0.5) * turn.z;
    Out o;
    o.rgbaq = int4(v.col);
    // XYZ2 in 12.4 fixed point: ftoi4 multiplies by 16 and makes integers
    o.xyz2 = vu::ftoi4(float4(p + offset.xy, 0, 1));
    return o;
}
