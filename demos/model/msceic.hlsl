// Model demo: mscei with ps2gl's SCEI clipping: a triangle of the strip with
// a vertex beyond the guard band (prm.w) in x, y or z is not drawn (ADC on
// its last vertex). The MVP makes z of the guard band's size too: -prm.w
// far away, prm.w at the nearest depth. The clipping flags go from vertex
// to vertex, so it is strips only (no indexed program).
#include "vu.hlsli"

cbuffer Uniforms {
    float4x4 mvp;
    float4 a[3];    // a[0] x + a[1] y + a[2] z = the three cosines (n of length 127)
    float4 e[3];    // light colours, 128 = 1.0
    float4 amb;     // w: alpha (the highlight of TFX HIGHLIGHT2: 0)
    float4 prm;     // xyz: the screen offset, w: the guard band
    float4 scl;     // xyz: the scale before the offset
};

struct In {
    float3 pos : POSITION;
    int4 nrm : NORMAL_V4_8;
    int2 uv : TEXCOORD_V2_16;
};

struct Out {
    float4 st : GIF_ST;
    int4 rgbaq : GIF_RGBAQ;
    int4 xyz2 : GIF_XYZ2;
};

[shader("vertex")]
Out msceic(In v) {
    float4 p = mul(mvp, float4(v.pos, 1));
    float q = rcp(p.w);
    float3 t = p.xyz * q;
    float3 n = float3(v.nrm.xyz);
    float3 c = max(a[0].xyz * n.x + a[1].xyz * n.y + a[2].xyz * n.z, (float3)0);
    float3 l = min(e[0].xyz * c.x + e[1].xyz * c.y + e[2].xyz * c.z + amb.xyz, (float3)255);
    // the triangle that ends here has a vertex beyond the guard band
    vu::clip(t, prm.w);
    Out o;
    o.st = float4(vu::itof12(v.uv) * q, q, 0);
    o.rgbaq = vu::rgba8(float4(l, amb.w));
    {
        // t first, as mscei: the same picture
#pragma clang fp reassociate(off)
        o.xyz2 = vu::ftoi4(float4(t * scl.xyz + prm.xyz, 0));
    }
    o.xyz2.w = vu::adc(vu::clipped(0x3FFFF));
    return o;
}
