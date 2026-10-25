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
// Below: the VU's fixed-point conversions (FTOI4/12/15, ITOF4/12/15), rgba8
// for the colour of GIF_RGBAQ, and clipping.

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
// A value that the GIF reads by its low bits (RGBAQ: 8 bits), rounded to an
// integer, for |v| < 2^22: v + 1.5 * 2^23 has round(v) in the low bits of
// its mantissa. One FMAC add, which joins an add or a clamp by a uniform or
// constant before it (int4(v) truncates, by an FTOI0 of its own).
int4 rgba8(float4 v) { return asint(v + 12582912.0); }

// Clipping (CLIPw, FCAND). clip judges v against +/-|w| and keeps the last
// four judgements in the clipping flags, six bits each (+x -x +y -y +z -z),
// the last in bits 0-5. clipped(mask) is 1 when a flag of mask is set, else
// 0: mask 0x3F the last vertex, 0x3FFFF the last three (the triangle of a
// strip that ends at it). Once per vertex: the flags go from vertex to
// vertex, and the first vertices of a batch see those of the batch before.
void __clip(float3 v, float w);
uint __fcand(uint mask);
void clip(float3 v, float w) { __clip(v, w); }
uint clipped(uint mask) { return __fcand(mask); }
// The w field of GIF_XYZ2 or GIF_XYZF2 that sets ADC (bit 15: no triangle
// ends at this vertex) when adc is 1 (a clipped result). Only bit 15 of w is
// read.
int adc(uint adc) { return int(adc) + 0x7FFF; }
} // namespace vu

#endif
