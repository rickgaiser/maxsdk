// tests/ps2/floor.c: floor on VU0, of a vector (x, -x, y, -y), and of the
// scalar z as fbm.hlsl takes it (floor(v.z) in field w of the result).
RWStructuredBuffer<float4> x;

[shader("compute")]
[numthreads(1, 1, 1)]
void fl(uint i : SV_DispatchThreadID) {
    float4 v = x[i];
    float4 f = floor(float4(v.x, -v.x, v.y, -v.y));
    float s = floor(v.z);
    x[i] = float4(f.xyz, s);
}
