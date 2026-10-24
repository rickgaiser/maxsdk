// procgen dust (PLAN_PROCGEN P4): motes that drift in a wind field, on VU0,
// every frame. x[i]: the position in box units (xyz, wrapped to 0..1; the box
// is laid around the eye when drawn, demos/dustdraw.hlsl), w: the mote's own
// phase. The wind is a swirl that changes with the place and the time, so
// the motes follow their own paths: a state that VU0 carries from frame to
// frame, not a function of the time alone.
cbuffer Params { float4 c; };   // dt * speed, the time, the swirl, the sink * dt
RWStructuredBuffer<float4> x;

[shader("compute")]
[numthreads(1, 1, 1)]
void dust(uint i : SV_DispatchThreadID) {
    float4 p = x[i];
    float4 a;
    {
        // The phases in the order of vuk's dust, a field each: the same
        // motes, bit for bit (HLSL may reassociate otherwise).
#pragma clang fp reassociate(off)
        a = float4(p.z, p.x, p.x + p.z, 0) * float4(9.0, 7.0, 5.0, 0) +
            c.y * float4(0.9, -0.6, 1.1, 0) + float4(p.w, p.w * 1.3, 0, 0);
    }
    float4 s = sin(a);
    float4 d = float4(s.x * c.z + 0.4, s.z * 0.3, s.y * c.z + 0.2, 0) * c.x - float4(0, c.w, 0, 0);
    x[i] = float4(p.xyz + d.xyz, p.w);
}
