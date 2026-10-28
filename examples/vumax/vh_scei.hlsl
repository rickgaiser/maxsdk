// vumax S6 in HLSL (the twin of vx_scei, ps2gl's SCEI shader): three
// directional lights clamped at 255, textured, and guard band clipping of
// the strip's triangles by ADC. The MVP gives offset-free pixels (prm.xyz
// is added before the 12.4 conversion); a triangle with a vertex beyond
// prm.w in x, y or z is not drawn (ADC on its last vertex).
#include "vu.hlsli"

cbuffer Uniforms {
    float4x4 mvp;
    float4 a[3];    // a[0] x + a[1] y + a[2] z = the three cosines
    float4 e[3];    // light colours
    float4 amb;     // w: alpha
    float4 prm;     // xyz: the screen offset, w: the guard band
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
Out vh_scei(In v) {
    float4 p = mul(mvp, float4(v.pos, 1));
    float q = rcp(p.w);
    float3 t = p.xyz * q;
    float3 c = max(a[0].xyz * v.nrm.x + a[1].xyz * v.nrm.y + a[2].xyz * v.nrm.z, (float3)0);
    float3 l = min(e[0].xyz * c.x + e[1].xyz * c.y + e[2].xyz * c.z + amb.xyz, (float3)255);
    // the triangle that ends here has a vertex beyond the guard band
    vu::clip(t, prm.w);
    Out o;
    o.st = float4(v.st * q, 0);   // (s q, t q, q)
    o.rgbaq = vu::rgba8(float4(l, amb.w));
    o.xyz2 = vu::ftoi4(float4(t + prm.xyz, 0));
    o.xyz2.w = vu::adc(vu::clipped(0x3FFFF));
    return o;
}
