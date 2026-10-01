// RTSky - calibration probe (cs_6_5, inline ray tracing)
// Traces primary rays for an 8x8 grid of pixels under 8 camera hypotheses and counts how many
// land at the distance the depth buffer reports. The host picks the hypothesis (camera latency,
// TLAS in world or camera-relative space) that matches best.
//   SRV t0 LinearDepth[cur]        UAV u0 RWStructuredBuffer<uint> ProbeResults[18]
//   TLAS t0 space1                 Dispatch (1, 1, 1)
#include "Common.hlsli"

RaytracingAccelerationStructure g_Scene : register(t0, space1);
Texture2D<float> g_Depth : register(t0);
RWStructuredBuffer<uint> g_Results : register(u0);

groupshared uint gs_Match[RTSKY_PROBE_HYPOTHESES];
groupshared uint gs_Valid[RTSKY_PROBE_HYPOTHESES];
groupshared uint gs_Pixels;

float ProbeTrace(float3 origin, float3 dir, float tMax)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = 0.05f;
    ray.TMax = tMax;
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(g_Scene, RAY_FLAG_NONE, g_Frame.traceFlags.y & 0xFFu, ray);
    q.Proceed();
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? q.CommittedRayT() : -1.0f;
}

[numthreads(RTSKY_PROBE_GRID, RTSKY_PROBE_GRID, 1)]
void ProbeCS(uint3 gtid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
    if (gi < RTSKY_PROBE_HYPOTHESES)
    {
        gs_Match[gi] = 0;
        gs_Valid[gi] = 0;
    }
    if (gi == 0)
        gs_Pixels = 0;
    GroupMemoryBarrierWithGroupSync();

    // Jittered cell position, inset from the screen border (first-person weapon, vignette).
    uint frame = g_Frame.traceFlags.z;
    uint h = PcgHash(gi * 7919u + frame * 104729u);
    float2 jitter = float2(UintToUnitFloat(h), UintToUnitFloat(PcgHash(h)));
    float2 uv = 0.1f + 0.8f * (float2(gtid.xy) + jitter) / float(RTSKY_PROBE_GRID);
    uint2 pixel = min(uint2(uv * g_Frame.traceSize.xy), uint2(g_Frame.traceSize.xy) - 1);
    uv = PixelToUV(pixel);

    float z = g_Depth.Load(int3(pixel, 0));
    if (z > 1.0f && z < 300.0f)
    {
        InterlockedAdd(gs_Pixels, 1);
        float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
        [loop]
        for (uint k = 0; k < RTSKY_PROBE_HYPOTHESES; ++k)
        {
            float4 offset = g_Frame.probeCam[k * 4 + 0];
            float3 right = g_Frame.probeCam[k * 4 + 1].xyz;
            float3 up = g_Frame.probeCam[k * 4 + 2].xyz;
            float3 fwd = g_Frame.probeCam[k * 4 + 3].xyz;
            if (dot(fwd, fwd) < 0.5f)
                continue; // hypothesis unavailable (not enough camera history yet)

            float3 ray = fwd + ndc.x * right + ndc.y * up;
            float rayLen = length(ray);
            float3 dir = ray / rayLen;
            float tDepth = z * rayLen;
            float3 origin = offset.w > 0.5f ? float3(0.0f, 0.0f, 0.0f) : g_Frame.camPos.xyz + offset.xyz;
            float t = ProbeTrace(origin, dir, tDepth * 1.5f + 10.0f);
            InterlockedAdd(gs_Valid[k], 1);
            if (t >= 0.0f && abs(t - tDepth) < 0.03f * tDepth)
                InterlockedAdd(gs_Match[k], 1);
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if (gi < RTSKY_PROBE_HYPOTHESES)
    {
        g_Results[gi] = gs_Match[gi];
        g_Results[RTSKY_PROBE_HYPOTHESES + gi] = gs_Valid[gi];
    }
    if (gi == 0)
    {
        g_Results[2 * RTSKY_PROBE_HYPOTHESES] = gs_Pixels;
        g_Results[2 * RTSKY_PROBE_HYPOTHESES + 1] = g_Frame.traceFlags.z; // stamp: frame index
    }
}
