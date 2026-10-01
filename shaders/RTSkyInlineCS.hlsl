// RTSky - trace pass using inline ray tracing (DXR 1.1 RayQuery, cs_6_5).
// Same inputs/outputs as the DXR pipeline (see TraceCommon.hlsli). Dispatch ceil(W/8) x ceil(H/8).
#include "TraceCommon.hlsli"

float TraceVisibility(float3 origin, float3 dir, float tMin, float tMax)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = tMin;
    ray.TMax = tMax;

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(g_Scene, VisibilityRayFlags() & ~RAY_FLAG_SKIP_CLOSEST_HIT_SHADER, InstanceMask(), ray);
    while (q.Proceed())
    {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
            FoliageAccept(q.CandidateInstanceIndex(), q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics()))
        {
            q.CommitNonOpaqueTriangleHit();
        }
    }
    return q.CommittedStatus() == COMMITTED_NOTHING ? 1.0f : 0.0f;
}

float TraceDistance(float3 origin, float3 dir, float tMin, float tMax)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = tMin;
    ray.TMax = tMax;

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(g_Scene, DistanceRayFlags(), InstanceMask(), ray);
    while (q.Proceed())
    {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
            FoliageAccept(q.CandidateInstanceIndex(), q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics()))
        {
            q.CommitNonOpaqueTriangleHit();
        }
    }
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? q.CommittedRayT() : -1.0f;
}

#include "TracePixel.hlsli"

[numthreads(RTSKY_GROUP_SIZE, RTSKY_GROUP_SIZE, 1)]
void TraceInlineCS(uint3 id : SV_DispatchThreadID)
{
    TracePixel(id.xy);
}
