// tests/ps2/vukloops.c: a run-time loop in a run-time loop, each pass over
// the same items: x[i] = x[i] * 3 + k, k = 1, 2, ... per pass.
cbuffer Params {
    uint passes;
};
RWStructuredBuffer<float4> x;

[shader("compute")]
[numthreads(1, 1, 1)]
void nest() {
    uint n, stride;
    x.GetDimensions(n, stride);
    float k = 1.0;
    for (uint p = 0; p < passes; p++) {
        for (uint i = 0; i < n; i++)
            x[i] = x[i] * 3 + k;
        k = k + 1;
    }
}
