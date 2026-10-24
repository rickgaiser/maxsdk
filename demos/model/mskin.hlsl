// Model demo: mtex with skinning. Each vertex has up to four joints (V4-8U:
// slots of the draw's palette) and their weights (V4-8U, summing to 255);
// the joint matrices are blended by the weights and transform the position
// and the normal from the bind pose into model space.
#include "vu.hlsli"

#define JOINTS 24

cbuffer Uniforms {
    float4x4 mvp;
    float4 light;   // towards the light in model space; w 0
    float4 lcol;    // light colour, 128 = 1.0; w 0
    float4 amb;     // ambient colour; w the alpha
    float4 joint[4 * JOINTS]; // joint j: the columns 4 j .. 4 j + 3 of its matrix
};

struct In {
    float3 pos : POSITION;
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
    float4 m = c0 * v.pos.x + c1 * v.pos.y + c2 * v.pos.z + c3;
    float3 n = (c0 * nrm.x + c1 * nrm.y + c2 * nrm.z).xyz;
    float4 p = mul(mvp, float4(m.xyz, 1));
    float q = rcp(p.w);
    float d = max(dot(n, light.xyz) * rsqrt(dot(n, n) + 1e-6), 0.0f);
    Out o;
    o.st = float4(vu::itof12(v.uv) * q, q, 0);
    o.rgbaq = int4(min(amb + lcol * d, (float4)255));
    o.xyz2 = vu::ftoi4(p * q);
    return o;
}
