// Model demo: Gouraud with one directional light (wrapped), ambient and a
// white specular highlight. The normal is V4-8 (signed, length 127).
#include "vu.hlsli"

// The light of a surface turned by n.l from it (-1 .. 1), wrapped around
// (half Lambert): 1 facing it, 1/4 side on, fading to 0 facing away, so the
// shading has no hard edge where the light ends.
float vu_wrap(float nl) {
    float h = nl * 0.5f + 0.5f;
    return h * h;
}

// Blinn-Phong: (n.h)^16, h half way between the directions to the light and
// to the eye (n, l, e of length 1).
float vu_spec(float3 n, float3 l, float3 e) {
    float3 h = l + e;
    float s = max(dot(n, h) * rsqrt(dot(h, h) + 1e-12f), 0.0f);
    s = s * s;
    s = s * s;
    s = s * s;
    return s * s;
}

cbuffer Uniforms {
    float4x4 mvp;
    float4 light;   // towards the light in model space, divided by 127; w 0
    float4 lcol;    // light colour times the material colour (0..255); w 0
    float4 amb;     // ambient colour times the material colour; w the alpha
    float4 eye;     // the eye in model space; w the highlight (0..255)
};

struct In {
    // w: 2056.0 where a strip restarts, else 0: its bits (0x45008000) and
    // its FTOI4 (0x8080) have bit 15, XYZ2's ADC (no triangle ends at
    // this vertex); V4-32 in strips, 1.0 from the VIF when the demo
    // sends V3-32 (indexed)
    float4 pos : POSITION_V3_32;
    int4 nrm : NORMAL_V4_8;
};

struct Out {
    int4 rgbaq : GIF_RGBAQ;
    int4 xyz2 : GIF_XYZ2;
};

[shader("vertex")]
Out mlit(In v) {
    float4 p = mul(mvp, float4(v.pos.xyz, 1));
    float3 n = float3(v.nrm.xyz);
    float d = vu_wrap(dot(n, light.xyz));
    float3 e = eye.xyz - v.pos.xyz;
    float s = eye.w * vu_spec(n * (1.0f / 127.0f), light.xyz * 127.0f,
                              e * rsqrt(dot(e, e) + 1e-12f));
    float4 c = amb + lcol * d;
    Out o;
    o.rgbaq = int4(min(float4(c.xyz + s, amb.w), (float4)255)); // RGBAQ keeps 8 bits
    o.xyz2 = vu::ftoi4(p / p.w);
    o.xyz2.w = asint(v.pos.w);
    return o;
}
