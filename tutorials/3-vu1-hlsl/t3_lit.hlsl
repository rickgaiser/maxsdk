// Tutorial 3, step 3: light. Each vertex has a normal, the direction its
// surface faces. The light is brightest where the normal points at the
// light: the dot product of the two, 1 there, 0 where the surface is side
// on (or turned away: max(.., 0)). The light's direction is in the model's
// own space (the EE turns it), so the normals need no transform.
#include "vu.hlsli"

cbuffer Uniforms {
    float4x4 mvp;
    float4 light;   // xyz: towards the light, length 1, in model space
    float4 colour;  // the surface lit full on (0 .. 255)
    float4 ambient; // the light from everywhere; w: alpha
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
Out t3_lit(In v) {
    float4 p = mul(mvp, float4(v.pos, 1));
    float d = max(dot(v.nrm, light.xyz), 0.0);
    Out o;
    // rgba8 rounds to the integers the GS reads (int4 would truncate)
    o.rgbaq = vu::rgba8(float4(ambient.xyz + colour.xyz * d, ambient.w));
    o.xyz2 = vu::ftoi4(p * rcp(p.w));
    return o;
}
