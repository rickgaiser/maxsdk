// vumax S3 in HLSL (the twin of vx_tex): S2 textured, perspective correct.
#include "vu.hlsli"

cbuffer Uniforms {
    float4x4 mvp;
};

struct In {
    float3 pos : POSITION;
    uint4 col : COLOR_V4_8U;
    // not float3 st : TEXCOORD_V2_32 (z = 1.0 from the VIF, one FMAC
    // instruction less): S3 waits for the VIF, and the masked UNPACK costs it
    // more than the instruction (PS2: 15.21 against 14.44 cycles per vertex)
    float2 st : TEXCOORD;
};

struct Out {
    float4 st : GIF_ST;
    int4 rgbaq : GIF_RGBAQ;
    int4 xyz2 : GIF_XYZ2;
};

[shader("vertex")]
Out vh_tex(In v) {
    float4 p = mul(mvp, float4(v.pos, 1));
    float q = rcp(p.w);
    Out o;
    o.st = float4(v.st * q, q, 0);
    o.rgbaq = int4(v.col);
    o.xyz2 = vu::ftoi4(p * q);
    return o;
}
