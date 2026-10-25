// vumax S5 in HLSL (the twin of vx_littex): S4 textured.
#include "vu.hlsli"

cbuffer Uniforms {
    float4x4 mvp;
    float4 d[3];
    float4 amb;     // w: alpha
};

struct In {
    float3 pos : POSITION;
    float3 nrm : NORMAL;
    float3 st : TEXCOORD_V2_32; // z: 1.0 from the VIF
};

struct Out {
    float4 st : GIF_ST;
    int4 rgbaq : GIF_RGBAQ;
    int4 xyz2 : GIF_XYZ2;
};

[shader("vertex")]
Out vh_littex(In v) {
    float4 p = mul(mvp, float4(v.pos, 1));
    float q = rcp(p.w);
    float3 l = max(d[0].xyz * v.nrm.x + d[1].xyz * v.nrm.y + d[2].xyz * v.nrm.z, (float3)0);
    Out o;
    o.st = float4(v.st * q, 0);   // (s q, t q, q)
    o.rgbaq = vu::rgba8(float4(l + amb.xyz, amb.w));
    o.xyz2 = vu::ftoi4(p * q);
    return o;
}
