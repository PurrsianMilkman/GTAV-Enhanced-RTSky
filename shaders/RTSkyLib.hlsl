// RTSky - DXR ray tracing pipeline (compiled as a DXIL library, lib_6_3)
//
// Traces world-space rays against the game's own top-level acceleration structure. The shader
// table has one raygen record, one miss record and one hit group record; the hit group table is
// bound with StrideInBytes = 0 so that every game instance, whatever its
// InstanceContributionToHitGroupIndex, resolves to RTSky's hit group.
//
// Exports: RayGen, Miss, ClosestHit, AnyHit; hit group "RTSkyHitGroup" (triangles) is created by
// the host (RtPipeline.cpp).
#include "TraceCommon.hlsli"

struct RayPayload
{
    float t; // < 0: miss, >= 0: hit distance (0 when the closest-hit shader was skipped)
};

float TraceVisibility(float3 origin, float3 dir, float tMin, float tMax)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = tMin;
    ray.TMax = tMax;
    RayPayload payload;
    payload.t = 0.0f;
    TraceRay(g_Scene, VisibilityRayFlags(), InstanceMask(), 0, 0, 0, ray, payload);
    return payload.t < 0.0f ? 1.0f : 0.0f;
}

float TraceDistance(float3 origin, float3 dir, float tMin, float tMax)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = tMin;
    ray.TMax = tMax;
    RayPayload payload;
    payload.t = -1.0f;
    TraceRay(g_Scene, DistanceRayFlags(), InstanceMask(), 0, 0, 0, ray, payload);
    return payload.t;
}

#include "TracePixel.hlsli"

[shader("raygeneration")]
void RayGen()
{
    TracePixel(DispatchRaysIndex().xy);
}

[shader("miss")]
void Miss(inout RayPayload payload)
{
    payload.t = -1.0f;
}

[shader("closesthit")]
void ClosestHit(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attribs)
{
    payload.t = RayTCurrent();
}

[shader("anyhit")]
void AnyHit(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attribs)
{
    // Only invoked for non-opaque (alpha-tested) geometry in stochastic foliage mode.
    if (!FoliageAccept(InstanceIndex(), PrimitiveIndex(), attribs.barycentrics))
        IgnoreHit();
}
