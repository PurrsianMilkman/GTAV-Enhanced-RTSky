// RTSky - composite: relight the game's HDR lighting buffer with ray-traced sky visibility.
//   SRV t0 FiltS[final], t1 FiltU[final], t2 LinearDepth[cur], t3 Normal[cur], t4 SkyData,
//       t5 HistMeta[cur], t6 TraceS
//   UAV u0 HDR colour (read-modify-write, typed UAV load)
//   Dispatch ceil(targetViewport.z / 8) x ceil(targetViewport.w / 8)
//
// The game's pixel colour is approximately albedo * (E_keep + E_sky * A) / pi, where E_keep is the
// light RTSky does not replace (sun/moon, ground bounce, artificial ambient) and E_sky * A the sky
// ambient the game assumes (A = GameSkyOcclusion). Replacing A by the ray-traced sky visibility R
// only needs the ratio (E_keep + E_sky * R) / (E_keep + E_sky * A) - the albedo cancels.
#include "Sky.hlsli"

Texture2D<float4> g_FiltS : register(t0);
Texture2D<float4> g_FiltU : register(t1);
Texture2D<float> g_Depth : register(t2);
Texture2D<float2> g_Normal : register(t3);
StructuredBuffer<SkyData> g_SkyData : register(t4);
Texture2D<float4> g_HistMeta : register(t5);
Texture2D<float4> g_TraceS : register(t6);
RWTexture2D<float4> g_Color : register(u0);

float3 SkyVisibilityRatio(float4 s, float4 u)
{
    float3 r;
    r.x = u.x > 1e-8f ? s.x / u.x : 1.0f;
    r.y = u.y > 1e-8f ? s.y / u.y : 1.0f;
    r.z = u.z > 1e-8f ? s.z / u.z : 1.0f;
    return saturate(r);
}

float3 EncodeForTarget(float3 linearValue)
{
    // Float HDR targets are linear. A UNORM target is assumed to be gamma 2.2 encoded.
    return g_Frame.compositeParams3.w > 0.5f ? pow(max(linearValue, 0.0f), 1.0f / 2.2f) : linearValue;
}

[numthreads(RTSKY_GROUP_SIZE, RTSKY_GROUP_SIZE, 1)]
void CompositeCS(uint3 id : SV_DispatchThreadID)
{
    uint2 local = id.xy;
    float2 vpSize = g_Frame.targetViewport.zw;
    if (any(float2(local) >= vpSize))
        return;
    uint2 target = local + uint2(g_Frame.targetViewport.xy);

    // Map the target pixel onto the trace grid (identical sizes in the common case).
    float2 uv = (float2(local) + 0.5f) / vpSize;
    uint2 tp = min(uint2(uv * g_Frame.traceSize.xy), uint2(g_Frame.traceSize.xy) - 1);

    float z = g_Depth.Load(int3(tp, 0));
    if (z <= 0.0f)
        return; // sky / no geometry: untouched

    uint view = (uint)g_Frame.compositeParams3.z;
    float4 color = g_Color[target];

    float4 s = g_FiltS.Load(int3(tp, 0));
    float4 u = g_FiltU.Load(int3(tp, 0));
    float3 R = SkyVisibilityRatio(s, u);
    float sunVisibility = saturate(s.a);
    float3 N = DecodeNormalOct(g_Normal.Load(int3(tp, 0)));

    SkyData sky = g_SkyData[0];
    float3 eSky = EvaluateSkyIrradianceSH(g_SkyData, N);
    // Same selector as the trace's shadow ray (sun until it is ~10 degrees below the horizon).
    bool lightIsSun = g_Frame.lightSelect.x > 0.5f;
    float3 lightDir = lightIsSun ? g_Frame.sunDir.xyz : g_Frame.moonDir.xyz;
    float3 eLight = lightIsSun ? sky.sunIrradiance.rgb : sky.moonIrradiance.rgb;
    float3 eDir = eLight * saturate(dot(N, lightDir)) * sunVisibility * g_Frame.compositeParams2.x;
    float3 eHoriz = sky.skyHorizontal.rgb + sky.sunIrradiance.rgb * saturate(g_Frame.sunDir.z)
                  + sky.moonIrradiance.rgb * saturate(g_Frame.moonDir.z);
    float3 eGround = g_Frame.compositeParams2.z * eHoriz * (0.5f - 0.5f * N.z);
    float3 eArt = g_Frame.compositeParams2.y.xxx;
    float3 eKeep = eDir + eGround + eArt;

    float A = g_Frame.compositeParams.w;
    float3 ratio = (eKeep + eSky * R) / max(eKeep + eSky * A, 1e-10f);
    ratio = clamp(ratio, g_Frame.compositeParams.y, g_Frame.compositeParams.z);

    // Fades: global (interiors, cutscenes...), near (first-person weapon / body), distance (fog, BVH range)
    float fade = saturate(g_Frame.compositeParams3.x);
    float nearFade = g_Frame.compositeParams3.y;
    if (nearFade > 0.0f)
        fade *= saturate(z / nearFade - 1.0f);
    float fadeStart = g_Frame.compositeParams4.x;
    float fadeEnd = g_Frame.compositeParams4.y;
    if (fadeEnd > fadeStart)
        fade *= 1.0f - saturate((z - fadeStart) / (fadeEnd - fadeStart));
    float strength = g_Frame.compositeParams.x * fade;

    float3 result;
    switch (view)
    {
    case RTSKY_VIEW_SKY_RATIO:
        result = EncodeForTarget(R);
        break;
    case RTSKY_VIEW_SUN_VIS:
        result = EncodeForTarget(sunVisibility.xxx);
        break;
    case RTSKY_VIEW_NORMALS:
        result = EncodeForTarget(N * 0.5f + 0.5f);
        break;
    case RTSKY_VIEW_DEPTH:
        result = EncodeForTarget(frac(log2(max(z, 1e-3f))).xxx);
        break;
    case RTSKY_VIEW_SKY_S:
        result = EncodeForTarget(s.rgb / max(Luminance(eSky) * INV_PI * TraceRadianceScale(), 1e-8f));
        break;
    case RTSKY_VIEW_TLAS:
        result = EncodeForTarget(g_TraceS.Load(int3(tp, 0)).rgb);
        break;
    case RTSKY_VIEW_RATIO:
        result = EncodeForTarget(ratio * 0.5f);
        break;
    case RTSKY_VIEW_HISTORY:
        result = EncodeForTarget((g_HistMeta.Load(int3(tp, 0)).x / max(g_Frame.temporalParams.x, 1.0f)).xxx);
        break;
    default:
    {
        float3 multiplier = lerp(float3(1.0f, 1.0f, 1.0f), ratio, strength);
        if (g_Frame.compositeParams3.w > 0.5f)
            multiplier = pow(multiplier, 1.0f / 2.2f);
        result = color.rgb * multiplier;
        break;
    }
    }

    // Split-screen compare (hotkey): left half untouched, right half as above, with a bright divider.
    if (g_Frame.compositeParams4.z > 0.5f)
    {
        float split = floor(vpSize.x * 0.5f);
        float x = float(local.x);
        if (x < split - 1.0f)
            result = color.rgb;
        else if (x <= split + 1.0f)
            result = color.rgb * 4.0f + 0.05f;
    }

    g_Color[target] = float4(result, color.a);
}
