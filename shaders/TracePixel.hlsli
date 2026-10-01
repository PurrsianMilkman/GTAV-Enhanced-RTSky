// RTSky - trace pass, part 2: per-pixel sky visibility estimator (see TraceCommon.hlsli).
#ifndef RTSKY_TRACE_PIXEL_HLSLI
#define RTSKY_TRACE_PIXEL_HLSLI

// Primary-ray agreement view: does the game's TLAS, seen through RTSky's camera, line up with the
// depth buffer? green = hit within 3% of depth, red = hit at a different distance, blue = no hit.
float4 DebugTlasAgreement(float2 uv, float viewZ)
{
    float3 ray = CameraRayCurrent(uv);
    float rayLen = length(ray);
    float3 dir = ray / rayLen;
    float tDepth = viewZ * rayLen;
    float t = TraceDistance(g_Frame.tlasOffset.xyz, dir, 0.05f, max(g_Frame.traceParams.x, tDepth * 1.5f + 10.0f));
    if (t < 0.0f)
        return float4(0.0f, 0.0f, 1.0f, 1.0f);
    float relErr = abs(t - tDepth) / max(tDepth, 1e-3f);
    return relErr < 0.03f ? float4(0.0f, 1.0f, 0.0f, 1.0f) : float4(1.0f, saturate(relErr * 2.0f), 0.0f, 1.0f);
}

void TracePixel(uint2 pixel)
{
    if (any(pixel >= uint2(g_Frame.traceSize.xy)))
        return;

    float viewZ = g_LinearDepth.Load(int3(pixel, 0));
    float2 uv = PixelToUV(pixel);

    if (g_Frame.traceFlags.w & RTSKY_FLAG_DEBUG_TLAS)
    {
        g_TraceS[pixel] = viewZ > 0.0f ? DebugTlasAgreement(uv, viewZ) : float4(0.0f, 0.0f, 0.0f, 1.0f);
        g_TraceU[pixel] = float4(0.0f, 0.0f, 0.0f, viewZ > 0.0f ? 1.0f : 0.0f);
        return;
    }

    if (viewZ <= 0.0f)
    {
        g_TraceS[pixel] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        g_TraceU[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    float3 N = DecodeNormalOct(g_Normal.Load(int3(pixel, 0)));
    float3 posRel = CameraRayCurrent(uv) * viewZ;
    float bias = g_Frame.traceParams.y + g_Frame.traceParams.z * viewZ;
    float3 origin = posRel + N * bias + g_Frame.tlasOffset.xyz;
    float tMin = g_Frame.traceParams.w;
    float tMax = g_Frame.traceParams.x;
    float4 overcast = g_SkyData[0].overcast;
    float radianceScale = TraceRadianceScale();

    uint rays = clamp(g_Frame.traceFlags.x, 1u, 16u);
    uint frame = g_Frame.traceFlags.z;

    // Near-field split: occluders closer than nearRadius are assumed to be handled already by the
    // game's own ambient occlusion (baked vertex AO, SSAO/RTAO). U is the sky radiance that survives
    // the near field, S the radiance that survives the whole ray. R = S / U is then the additional,
    // large-scale sky occlusion (bridges, overhangs, canopies, street canyons). With nearRadius = 0
    // U is the unoccluded sky radiance and R is the full sky visibility.
    float nearRadius = g_Frame.foliageParams.z;

    float3 S = 0.0f;
    float3 U = 0.0f;
    for (uint i = 0; i < rays; ++i)
    {
        float2 u = SampleR2(pixel, frame, i);
        float3 dir = CosineSampleHemisphere(N, u);
        float3 L = SkyRadiance(g_SkyViewLut, dir, overcast) * radianceScale;
        if (dir.z > 0.0f)
        {
            float vNear = 1.0f;
            if (nearRadius > tMin)
                vNear = TraceVisibility(origin, dir, tMin, nearRadius);
            float vFar = 0.0f;
            if (vNear > 0.0f)
                vFar = TraceVisibility(origin, dir, max(tMin, nearRadius), tMax);
            U += L * vNear;
            S += L * vFar;
        }
    }
    S /= float(rays);
    U /= float(rays);

    float sunVisibility = 1.0f;
    if (g_Frame.traceFlags.w & RTSKY_FLAG_SUN_RAYS)
    {
        bool useSun = g_Frame.lightSelect.x > 0.5f;
        float3 lightDir = useSun ? g_Frame.sunDir.xyz : g_Frame.moonDir.xyz;
        float coneHalfAngle = useSun ? g_Frame.sunDir.w : g_Frame.moonDir.w;
        if (dot(N, lightDir) > 0.0f && lightDir.z > -0.05f)
        {
            float2 u = SampleR2(pixel, frame, rays + 7u);
            float3 dir = SampleCone(lightDir, cos(coneHalfAngle), u);
            sunVisibility = TraceVisibility(origin, dir, tMin, tMax * 4.0f);
        }
        else
        {
            sunVisibility = 0.0f;
        }
    }

    g_TraceS[pixel] = float4(S, sunVisibility);
    g_TraceU[pixel] = float4(U, 1.0f);
}

#endif // RTSKY_TRACE_PIXEL_HLSLI
