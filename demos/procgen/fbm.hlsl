// Terrain heights (PLAN_PROCGEN P3): fractal value noise, oct octaves, on
// VU0. x[i] = (x, z, 0, _) in lattice units in; the height, about -1..1, is
// x[i].z out (x and y are the coordinates of a next octave).
//
// The lattice hash is Dave Hoskins' hash12 ("hash without sine"), on the four
// corners of a cell at once, one per field: no tables, only FMAC
// instructions. Between the corners: smoothstep.
//
// The octaves are the outer loop and the points the inner one: each octave
// is a loop over independent points, which the compiler pipelines, with the
// sum and the coordinates kept in x[i] between octaves.
// The hash takes the fraction of large products: the operations keep their
// order (HLSL lets the compiler reassociate them), or the hash changes.
#pragma clang fp reassociate(off)

cbuffer Params { uint oct; };
RWStructuredBuffer<float4> x;

[shader("compute")]
[numthreads(1, 1, 1)]
void fbm() {
    uint n, stride;
    x.GetDimensions(n, stride);
    float amp = 0.5;
    for (uint o = 0; o < oct; o++) {
        for (uint i = 0; i < n; i++) {
            float4 v = x[i];
            float px = v.x, py = v.y;
            float ix = floor(px), iy = floor(py);
            float fx = px - ix, fy = py - iy;
            float4 cx = ix + float4(0, 1, 0, 1);
            float4 cy = iy + float4(0, 0, 1, 1);
            float4 a = frac(cx * 0.1031);
            float4 b = frac(cy * 0.1031);
            float4 d = a * (b + 33.33) + b * (a + 33.33) + a * (a + 33.33);
            float4 h = frac((a + d + b + d) * (a + d));
            float sx = fx * fx * (3 - 2 * fx);
            float sy = fy * fy * (3 - 2 * fy);
            float h0 = lerp(h.x, h.y, sx);
            float h1 = lerp(h.z, h.w, sx);
            float sum = v.z + (lerp(h0, h1, sy) * 2 - 1) * amp;
            float nx = (px * 0.8 - py * 0.6) * 2 + 17.1;
            float ny = (px * 0.6 + py * 0.8) * 2 + 3.7;
            x[i] = float4(nx, ny, sum, sum);
        }
        amp = amp * 0.5;
    }
}
