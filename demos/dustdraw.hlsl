// procgen dust (PLAN_PROCGEN P4), drawn: each mote (demos/dust.hlsl, box
// units 0..1) is moved into the box of size box.w laid around the eye, and
// drawn as a small fogged, alpha-blended sprite. A mote near the eye or
// reaching out of the guard band is not drawn (ADC on its second vertex).
#include "vu.hlsli"

cbuffer Uniforms {
    float4x4 mvp;   // for the box's corner (x, z) at y = 0
    float4 fogc;
    float4 box;     // xyz: the box's corner in box units, w: its size
    float4 ext;     // x: the corner's height, y, z: half size in pixels at w = 1
    float4 col;     // colour 0..255, alpha in w
    float4 cull;    // x: the centre of the screen, y: guard band, z: the nearest w
};

struct In {
    float4 pos : POSITION;
};

struct Out {
    int4 rgbaq : GIF_RGBAQ;
    int4 xyzf20 : GIF_XYZF2_A;
    int4 xyzf21 : GIF_XYZF2_B;
};

[shader("vertex")]
Out dustdraw(In v) {
    float4 q = v.pos - box;
    q = q - floor(q);
    float4 wp = q * box.w;
    float4 p = mul(mvp, float4(wp.x, wp.y + ext.x, wp.z, 1));
    float r = rcp(p.w);
    float4 c = p * r;
    float fog = clamp(p.w * fogc.x + fogc.y, 0.0f, 255.0f);
    float hw = r * ext.y;
    float hh = r * ext.z;
    float ex = max(max(abs(c.x - cull.x) + hw, abs(c.y - cull.x) + hh) - cull.y, cull.z - p.w);
    float adc = clamp(ex * 1000000000.0, 0.0f, 2048.0f);
    Out o;
    o.rgbaq = int4(col);
    o.xyzf20 = vu::ftoi4(float4(c.x - hw, c.y - hh, c.z, fog));
    o.xyzf21 = vu::ftoi4(float4(c.x + hw, c.y + hh, c.z, fog + adc));
    return o;
}
