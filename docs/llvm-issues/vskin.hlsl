// Model demo: skinning on VU0 for the VU1 programs. A batch is the vertices
// of one cluster (m, a multiple of 4, up to 32), unpacked by VIF0 from the
// model's streams, four quadwords each: the bind pose position (V3-32), the
// normal (V4-8, length 127), the joints (V4-8U: palette slot * 3, the
// address of its first column) and the weights (V4-8U, summing to 255).
//
// Out, after the inputs, VIF1 packets ready for VU1, which toSPR copies into
// the frame's stream: tag[0], the positions as V3-32 (four vertices in three
// quadwords), tag[1], the normals as V4-8 (four in a quadword; each field
// n + 128, which VIF1 unpacks unsigned with ROW -128 added: STMOD offset),
// tag[2] (STMOD back to 0). The EE writes the tags (DMA tag and VIF codes).
#include "vu.hlsli"

cbuffer Skin {
    float4 pal[72]; // 24 joints: their three columns, the translation in w
    float4 tag[3];
};
RWStructuredBuffer<float4> x;

[shader("compute")]
[numthreads(1, 1, 1)]
void vskin() {
    uint n, stride;
    x.GetDimensions(n, stride);
    uint m = n / 4;
    uint pos = n + 1, nrm = n + 2 + m / 4 * 3;
    x[n] = tag[0];
    x[nrm - 1] = tag[1];
    x[nrm + m / 4] = tag[2];
    for (uint g = 0; g < m / 4; g++) {
        float3 p[4];
        float4 u;
        [unroll] for (uint k = 0; k < 4; k++) {
            uint b = 16 * g + 4 * k;
            float3 v = x[b].xyz;
            float3 nv = float3(asint(x[b + 1].xyz));
            uint4 j = asuint(x[b + 2]);
            float4 w = float4(asint(x[b + 3])) * (1.0 / 255.0); // bytes: no unsigned fix-up
            float4 c0 = pal[j.x] * w.x + pal[j.y] * w.y + pal[j.z] * w.z + pal[j.w] * w.w;
            float4 c1 = pal[j.x + 1] * w.x + pal[j.y + 1] * w.y + pal[j.z + 1] * w.z +
                        pal[j.w + 1] * w.w;
            float4 c2 = pal[j.x + 2] * w.x + pal[j.y + 2] * w.y + pal[j.z + 2] * w.z +
                        pal[j.w + 2] * w.w;
            p[k] = c0.xyz * v.x + c1.xyz * v.y + c2.xyz * v.z + float3(c0.w, c1.w, c2.w);
            float3 q = c0.xyz * nv.x + c1.xyz * nv.y + c2.xyz * nv.z;
            // n + 128 rounded (VU adds truncate: the magic number floors)
            float3 r;
            {
#pragma clang fp reassociate(off)
                r = (q * (127.0 * rsqrt(dot(q, q))) + (128.5 + 12582912.0)) - 12582912.0;
            }
            u[k] = r.x + r.y * 256.0 + r.z * 65536.0;
        }
        x[pos + 3 * g] = float4(p[0], p[1].x);
        x[pos + 3 * g + 1] = float4(p[1].yz, p[2].xy);
        x[pos + 3 * g + 2] = float4(p[2].z, p[3]);
        x[nrm + g] = asfloat(int4(u));
    }
}
