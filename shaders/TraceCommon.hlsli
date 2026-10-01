// RTSky - trace pass, part 1: resources and helpers shared by the DXR and RayQuery paths.
// Include order in a trace shader:
//   #include "TraceCommon.hlsli"
//   float TraceVisibility(float3 origin, float3 dir, float tMin, float tMax)  (1 = unoccluded)
//   float TraceDistance(float3 origin, float3 dir, float tMin, float tMax)    (-1 = miss)
//   #include "TracePixel.hlsli"
//
//   SRV t0 = LinearDepth[cur], t1 = Normal[cur], t2 = SkyViewLut, t3 = SkyData
//   UAV u0 = TraceS, u1 = TraceU       TLAS: t0, space1
#ifndef RTSKY_TRACE_COMMON_HLSLI
#define RTSKY_TRACE_COMMON_HLSLI

#include "Sky.hlsli"

RaytracingAccelerationStructure g_Scene : register(t0, space1);
Texture2D<float> g_LinearDepth : register(t0);
Texture2D<float2> g_Normal : register(t1);
Texture2D<float4> g_SkyViewLut : register(t2);
StructuredBuffer<SkyData> g_SkyData : register(t3);
RWTexture2D<float4> g_TraceS : register(u0);
RWTexture2D<float4> g_TraceU : register(u1);

uint VisibilityRayFlags()
{
    uint flags = RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER |
                 RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES;
    uint mode = g_Frame.traceFlags.w;
    if (mode & RTSKY_FLAG_FOLIAGE_OPAQUE)
        flags |= RAY_FLAG_FORCE_OPAQUE;
    else if (mode & RTSKY_FLAG_FOLIAGE_IGNORE)
        flags |= RAY_FLAG_CULL_NON_OPAQUE;
    return flags;
}

uint DistanceRayFlags()
{
    uint flags = RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES;
    uint mode = g_Frame.traceFlags.w;
    if (mode & RTSKY_FLAG_FOLIAGE_OPAQUE)
        flags |= RAY_FLAG_FORCE_OPAQUE;
    else if (mode & RTSKY_FLAG_FOLIAGE_IGNORE)
        flags |= RAY_FLAG_CULL_NON_OPAQUE;
    return flags;
}

uint InstanceMask()
{
    return g_Frame.traceFlags.y & 0xFFu;
}

// Stochastic but temporally stable coverage for non-opaque (alpha-tested) triangles. The game's
// alpha textures are not reachable from RTSky's shaders, so each triangle is split into a grid of
// cells in barycentric space and each cell is opaque with probability FoliageOpacity. This gives
// leaf-like partial occlusion under tree canopies instead of solid cards or no shadow at all.
bool FoliageAccept(uint instanceIndex, uint primitiveIndex, float2 barycentrics)
{
    float cells = max(g_Frame.foliageParams.y, 1.0f);
    uint2 cell = uint2(saturate(barycentrics) * cells);
    uint h = PcgHash(instanceIndex * 0x9E3779B1u ^ PcgHash(primitiveIndex ^ PcgHash(cell.x + cell.y * 131u)));
    return UintToUnitFloat(h) < g_Frame.foliageParams.x;
}

#endif // RTSKY_TRACE_COMMON_HLSLI
