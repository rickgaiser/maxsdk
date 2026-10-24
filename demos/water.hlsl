// procgen water (PLAN_PROCGEN): a flat grid moved by two travelling sine
// waves on VU1; the colour follows the waves, alpha blended, fogged. The
// input is the grid point in chunk coordinates (y = the water level).
// Culling as in lvl3: a triangle with a vertex outside the guard band or
// near the eye is not drawn (ADC on its last vertex).
#include "vu.hlsli"

cbuffer Uniforms {
    float4x4 mvp;
    float4 fogc;
    float4 wave;    // x: phase (time), y: amplitude, z, w: chunk origin x, z
    float4 wcol;    // base colour 0..255, alpha in w
    float4 cull;    // x: the centre of the screen, y: guard band, z: the nearest w
};

struct In {
    float3 pos : POSITION;
};

struct Out {
    int4 rgbaq : GIF_RGBAQ;
    int4 xyzf2 : GIF_XYZF2;
};

// x: the excess of the vertex before, y: of the one before that
static float2 exs = 0;

[shader("vertex")]
Out water(In v) {
    float wx = v.pos.x + wave.z;
    float wz = v.pos.z + wave.w;
    float a1, a2;
    {
        // The phases summed in the order of the source: the same waves as
        // vuk's, bit for bit (HLSL may reassociate otherwise).
#pragma clang fp reassociate(off)
        a1 = wx * 0.11 + wz * 0.07 + wave.x;
        a2 = wz * 0.13 - wx * 0.05 + wave.x * 1.3;
    }
    float s1 = sin(a1);
    float s2 = sin(a2);
    float y = (s1 + s2 * 0.6) * wave.y;
    float4 p = mul(mvp, float4(v.pos.x, v.pos.y + y, v.pos.z, 1));
    float fog = clamp(p.w * fogc.x + fogc.y, 0.0f, 255.0f);
    float4 c = wcol + (s1 + s2) * 14;
    float4 o4 = abs(p - p.w * cull.x) - p.w * cull.y;
    float ex = max(max(o4.x, o4.y), cull.z - p.w);
    float adc = clamp(max(ex, max(exs.x, exs.y)) * 1000000000.0, 0.0f, 2048.0f);
    exs = float2(ex, exs.x);
    Out o;
    o.rgbaq = int4(float4(c.xyz, wcol.w));
    o.xyzf2 = vu::ftoi4(float4((p / p.w).xyz, fog + adc));
    return o;
}
