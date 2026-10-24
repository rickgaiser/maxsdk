// vu.hlsli: HLSL for the PS2's vector units (maxsdk, llvm VUEntryABI.md).
//
// A program is compiled by clang for SPIR-V and then for the VU (see the
// Makefile): each [shader("vertex")] or [shader("compute")] function is a
// micro program. The PS2's additions are HLSL:
//
//  * vertex inputs: the UNPACK format is a suffix of the semantic,
//      uint4 col : COLOR_V4_8U;   int2 uv : TEXCOORD_V2_16;
//    without one, the 32-bit format of the type (float3: V3_32);
//  * vertex outputs are GIF registers, in the order of the struct:
//      GIF_RGBAQ GIF_ST GIF_UV GIF_XYZ2 GIF_XYZF2 GIF_FOG GIF_XYZ3 GIF_XYZF3;
//  * a compute program has one structured buffer, its items: per item with
//    SV_DispatchThreadID, or once per batch with its own loops over
//    GetDimensions items;
//  * a static variable that the program writes keeps its value from vertex
//    to vertex (item to item) of a batch;
//  * int and uint are 16 bits on the VU.
//
// Below: the VU's fixed-point conversions (FTOI4/12/15, ITOF4/12/15).

#ifndef VU_HLSLI
#define VU_HLSLI

namespace vu {
int4 ftoi4(float4 v) { return int4(v * 16.0); }
int4 ftoi12(float4 v) { return int4(v * 4096.0); }
int4 ftoi15(float4 v) { return int4(v * 32768.0); }
float4 itof4(int4 v) { return float4(v) * (1.0 / 16.0); }
float4 itof12(int4 v) { return float4(v) * (1.0 / 4096.0); }
float4 itof15(int4 v) { return float4(v) * (1.0 / 32768.0); }
float2 itof12(int2 v) { return float2(v) * (1.0 / 4096.0); }
} // namespace vu

#endif
