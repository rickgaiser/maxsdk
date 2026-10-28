// vumax S1 in HLSL (the twin of vx_flat): MVP and the perspective divide,
// XYZ2 only (the colour is set once per draw).
#include "vu.hlsli"

cbuffer Uniforms {
    float4x4 mvp;
};

struct In {
    float3 pos : POSITION;
};

struct Out {
    int4 xyz2 : GIF_XYZ2;
};

[shader("vertex")]
Out vh_flat(In v) {
    float4 p = mul(mvp, float4(v.pos, 1));
    Out o;
    o.xyz2 = vu::ftoi4(p * rcp(p.w));
    return o;
}
