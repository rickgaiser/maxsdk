// Model demo: ps2gl's SCEI shader (examples/vumax/vh_scei.hlsl) for the model's
// streams, without clipping: three directional lights clamped at 255,
// textured, perspective correct. The normal is V4-8 (length 127), the texture
// coordinates V2-16 in 4.12 fixed point. The MVP gives offset-free
// coordinates; the GS ones are t * scl + prm (msceic.hlsl: the same, clipped
// to the guard band).
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
    // w: 2056.0 where a strip restarts, else 0: its bits (0x45008000) and
    // its FTOI4 (0x8080) have bit 15, XYZ2's ADC (no triangle ends at
    // this vertex); V4-32 in strips, 1.0 from the VIF when the demo
    // sends V3-32 (indexed)
    float4 pos : POSITION_V3_32;
    int4 nrm : NORMAL_V4_8;
    int3 uv : TEXCOORD_V2_16_F4096; // z = 4096 (1.0) from the VIF: (s q, t q, q)
};

struct Out {
    float4 st : GIF_ST;
    int4 rgbaq : GIF_RGBAQ;
    int4 xyz2 : GIF_XYZ2;
};

[shader("vertex")]
Out mscei(In v) {
    float4 p = mul(mvp, float4(v.pos.xyz, 1));
    float q = rcp(p.w);
    float3 t = p.xyz * q;
    float3 n = float3(v.nrm.xyz);
    float3 c = max(a[0].xyz * n.x + a[1].xyz * n.y + a[2].xyz * n.z, (float3)0);
    float3 l = min(e[0].xyz * c.x + e[1].xyz * c.y + e[2].xyz * c.z + amb.xyz, (float3)255);
    Out o;
    o.st = float4(vu::itof12(v.uv) * q, 0);
    o.rgbaq = vu::rgba8(float4(l, amb.w));
    {
        // t first, as msceic (which clips t): the same picture
#pragma clang fp reassociate(off)
        o.xyz2 = vu::ftoi4(float4(t * scl.xyz + prm.xyz, 0));
    }
    o.xyz2.w = asint(v.pos.w);
    return o;
}
