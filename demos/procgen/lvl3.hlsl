// Quality level 3 (PLAN_PROCGEN): near terrain. Three directional lights by a
// light matrix, fog, and clipping for triangle strips: a triangle that has a
// vertex outside the guard band or near the eye is not drawn. Its last
// vertex gets ADC, which the vertex finds from its own excess and that of the
// two vertices before it (a static: it keeps its value from vertex to vertex).
#include "vu.hlsli"

cbuffer Uniforms {
    float4x4 mvp;
    float4 lm[3];   // lm[0] x + lm[1] y + lm[2] z = (n.L0, n.L1, n.L2) for n * 127;
                    // lm[0].w: the nearest w
    float4 lc[4];   // colours of the lights, lc[3] ambient (w 1 keeps alpha)
    float4 prm;     // fog = clamp(w * x + y, 0, 255) (255 none, 0 all fog);
                    // z: the centre of the screen in GS coordinates;
                    // w: guard band |X - z|, |Y - z| < w w
};

struct In {
    float3 pos : POSITION;
    int4 nrm : NORMAL_V4_8;
    uint4 col : COLOR_V4_8U;
    int2 uv : TEXCOORD_V2_16;
};

struct Out {
    float4 st : GIF_ST;
    int4 rgbaq : GIF_RGBAQ;
    int4 xyzf2 : GIF_XYZF2;
};

// x: the excess of the vertex before, y: of the one before that
static float2 exs = 0;

[shader("vertex")]
Out lvl3(In v) {
    float4 p = mul(mvp, float4(v.pos, 1));
    float q = rcp(p.w);
    float4 n = float4(v.nrm);
    float4 d = max(lm[0] * n.x + lm[1] * n.y + lm[2] * n.z, (float4)0);
    float4 light = lc[3] + lc[0] * d.x + lc[1] * d.y + lc[2] * d.z;
    float fog = clamp(p.w * prm.x + prm.y, 0.0f, 255.0f);
    // outside: |X - c| - k w > 0, |Y - c| - k w > 0 or w < near. 2048 sets
    // ADC after ftoi4; any positive excess gives the full 2048.
    float4 o4 = abs(p - p.w * prm.z) - p.w * prm.w;
    float ex = max(max(o4.x, o4.y), lm[0].w - p.w);
    float adc = clamp(max(ex, max(exs.x, exs.y)) * 1000000000.0, 0.0f, 2048.0f);
    exs = float2(ex, exs.x);
    Out o;
    o.st = float4(vu::itof12(v.uv) * q, q, 0);
    o.rgbaq = int4(min(float4(v.col) * light, (float4)255)); // RGBAQ keeps 8 bits
    o.xyzf2 = vu::ftoi4(float4((p * q).xyz, fog + adc));
    return o;
}
