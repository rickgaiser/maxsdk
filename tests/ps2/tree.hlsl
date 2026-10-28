// procgen trees (PLAN_PROCGEN): one textured sprite per tree, standing on its
// point, sized by the distance. Two GS vertices per input point (SPRITE).
// A sprite that reaches outside the guard band or a tree near the eye is not
// drawn (ADC on its second vertex).
#include "vu.hlsli"

cbuffer Uniforms {
    float4x4 mvp;
    float4 fogc;
    float4 bb;      // x: half width * fx, y: height * fy (pixels at w = 1)
    float4 uvc;     // texture corners u0, v0, u1, v1 (texels)
    float4 cull;    // x: the centre of the screen, y: guard band, z: the nearest w
};

struct In {
    float3 pos : POSITION;
};

struct Out {
    int4 uv0 : GIF_UV_A;
    int4 xyzf20 : GIF_XYZF2_A;
    int4 uv1 : GIF_UV_B;
    int4 xyzf21 : GIF_XYZF2_B;
};

[shader("vertex")]
Out tree(In v) {
    float4 p = mul(mvp, float4(v.pos, 1));
    float q = rcp(p.w);
    float4 c = p * q;
    float fog = clamp(p.w * fogc.x + fogc.y, 0.0f, 255.0f);
    float hw = q * bb.x;
    float h = q * bb.y;
    float ex = max(max(abs(c.x - cull.x) + hw, abs(c.y - cull.x) + h) - cull.y, cull.z - p.w);
    float adc = clamp(ex * 1000000000.0, 0.0f, 2048.0f);
    Out o;
    o.uv0 = vu::ftoi4(float4(uvc.x, uvc.y, 0, 0));
    o.xyzf20 = vu::ftoi4(float4(c.x - hw, c.y - h, c.z, fog));
    o.uv1 = vu::ftoi4(float4(uvc.z, uvc.w, 0, 0));
    o.xyzf21 = vu::ftoi4(float4(c.x + hw, c.y, c.z, fog + adc));
    return o;
}
