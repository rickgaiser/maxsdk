// Model demo: mtex with skinning. Each vertex has up to four joints (V4-8U:
// slots of the draw's palette) and their weights (V4-8U, summing to 255);
// the joint matrices are blended by the weights and transform the position
// and the normal from the bind pose into model space. The position is V3-16,
// 1 / 32768 of the model's size from its middle (demos/model/mxm.h): the
// joint matrices have the size and the middle folded in.
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

#define JOINTS 24

cbuffer Uniforms {
    float4x4 mvp;
    float4 light;   // towards the light in model space; w 0
    float4 lcol;    // light colour, 128 = 1.0; w 0
    float4 amb;     // ambient colour; w unused
    float4 eye;     // the eye in model space; w the highlight (0..255), as mtex
    float4 joint[4 * JOINTS]; // joint j: the columns 4 j .. 4 j + 3 of its matrix
};

struct In {
    int4 pos : POSITION_V3_16;
    int4 nrm : NORMAL_V4_8;
    int2 uv : TEXCOORD_V2_16;
    uint4 jnt : JOINTS_V4_8U;
    uint4 wgt : WEIGHTS_V4_8U;
};

struct Out {
    float4 st : GIF_ST;
    int4 rgbaq : GIF_RGBAQ;
    int4 xyz2 : GIF_XYZ2;
};

[shader("vertex")]
Out mskin(In v) {
    float4 w = float4(v.wgt) * (1.0 / 255.0);
    uint4 j = v.jnt * 4;
    float4 c0 = joint[j.x] * w.x + joint[j.y] * w.y + joint[j.z] * w.z + joint[j.w] * w.w;
    float4 c1 = joint[j.x + 1] * w.x + joint[j.y + 1] * w.y + joint[j.z + 1] * w.z + joint[j.w + 1] * w.w;
    float4 c2 = joint[j.x + 2] * w.x + joint[j.y + 2] * w.y + joint[j.z + 2] * w.z + joint[j.w + 2] * w.w;
    float4 c3 = joint[j.x + 3] * w.x + joint[j.y + 3] * w.y + joint[j.z + 3] * w.z + joint[j.w + 3] * w.w;
    float4 nrm = float4(v.nrm);
    float4 pos = vu::itof15(v.pos);
    float4 m = c0 * pos.x + c1 * pos.y + c2 * pos.z + c3;
    float3 n = (c0 * nrm.x + c1 * nrm.y + c2 * nrm.z).xyz;
    float4 p = mul(mvp, float4(m.xyz, 1));
    float q = rcp(p.w);
    n = n * rsqrt(dot(n, n) + 1e-6);
    float d = vu_wrap(dot(n, light.xyz));
    float3 e = eye.xyz - m.xyz;
    float s = eye.w * vu_spec(n, light.xyz, e * rsqrt(dot(e, e) + 1e-12f));
    float4 c = amb + lcol * d;
    Out o;
    o.st = float4(vu::itof12(v.uv) * q, q, 0);
    o.rgbaq = int4(min(float4(c.xyz, s), (float4)255));
    o.xyz2 = vu::ftoi4(p * q);
    return o;
}
