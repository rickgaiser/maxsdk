// vumax S4 in HLSL (the twin of vx_lit): one directional light. d[k] is the
// diffuse colour times component k of the direction to the light.
#include "vu.hlsli"

cbuffer Uniforms {
    float4x4 mvp;
    float4 d[3];
    float4 amb;     // w: alpha
};

struct In {
    float3 pos : POSITION;
    float3 nrm : NORMAL;
};

struct Out {
    int4 rgbaq : GIF_RGBAQ;
    int4 xyz2 : GIF_XYZ2;
};

[shader("vertex")]
Out vh_lit(In v) {
    float4 p = mul(mvp, float4(v.pos, 1));
    float3 l = max(d[0].xyz * v.nrm.x + d[1].xyz * v.nrm.y + d[2].xyz * v.nrm.z, (float3)0);
    Out o;
    o.rgbaq = vu::rgba8(float4(l + amb.xyz, amb.w));
    o.xyz2 = vu::ftoi4(p * rcp(p.w));
    return o;
}
