// RTSky - atmosphere LUT passes (Hillaire 2020)
//   TransmittanceCS  UAV u0 = TransmittanceLut (256x64)                    dispatch (32, 8, 1)
//   MultiScatterCS   SRV t0 = TransmittanceLut, UAV u0 = MultiScatterLut   dispatch (32, 32, 1)
//   SkyViewCS        SRV t0 = TransmittanceLut, t1 = MultiScatterLut,
//                    UAV u0 = SkyViewLut (192x108)                         dispatch (24, 14, 1)
#include "Atmosphere.hlsli"

Texture2D<float4> g_TransmittanceLut : register(t0);
Texture2D<float4> g_MultiScatterLut : register(t1);
RWTexture2D<float4> g_Output : register(u0);

[numthreads(8, 8, 1)]
void TransmittanceCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= RTSKY_TRANSMITTANCE_W || id.y >= RTSKY_TRANSMITTANCE_H)
        return;

    AtmosphereParameters atmo = GetAtmosphere();
    float2 uv = (float2(id.xy) + 0.5f) / float2(RTSKY_TRANSMITTANCE_W, RTSKY_TRANSMITTANCE_H);
    float viewHeight, viewZenithCos;
    UvToTransmittanceLutParams(atmo, uv, viewHeight, viewZenithCos);

    float3 worldPos = float3(0.0f, 0.0f, viewHeight);
    float3 worldDir = float3(0.0f, sqrt(saturate(1.0f - viewZenithCos * viewZenithCos)), viewZenithCos);
    float3 transmittance = exp(-IntegrateOpticalDepth(worldPos, worldDir, atmo));
    g_Output[id.xy] = float4(transmittance, 1.0f);
}

#define MS_SQRT_SAMPLES 8
groupshared float3 gs_MultiScatAs1[64];
groupshared float3 gs_L[64];

[numthreads(1, 1, 64)]
void MultiScatterCS(uint3 id : SV_DispatchThreadID)
{
    AtmosphereParameters atmo = GetAtmosphere();
    const float res = RTSKY_MULTISCATTER_SIZE;
    float2 uv = (float2(id.xy) + 0.5f) / res;
    uv = float2(FromSubUvsToUnit(uv.x, res), FromSubUvsToUnit(uv.y, res));

    float cosLightZenith = uv.x * 2.0f - 1.0f;
    float3 lightDir = float3(0.0f, sqrt(saturate(1.0f - cosLightZenith * cosLightZenith)), cosLightZenith);
    float viewHeight = atmo.BottomRadius + saturate(uv.y + PLANET_RADIUS_OFFSET) *
                       (atmo.TopRadius - atmo.BottomRadius - PLANET_RADIUS_OFFSET);
    float3 worldPos = float3(0.0f, 0.0f, viewHeight);

    const float sphereSolidAngle = 4.0f * PI;
    const float isotropicPhase = 1.0f / sphereSolidAngle;
    const float sqrtSamples = float(MS_SQRT_SAMPLES);

    uint z = id.z;
    float i = 0.5f + float(z / MS_SQRT_SAMPLES);
    float j = 0.5f + float(z % MS_SQRT_SAMPLES);
    float randA = i / sqrtSamples;
    float randB = j / sqrtSamples;
    float theta = 2.0f * PI * randA;
    float phi = acos(1.0f - 2.0f * randB);
    float3 worldDir = float3(cos(theta) * sin(phi), sin(theta) * sin(phi), cos(phi));

    ScatteringResult r = IntegrateScatteredLuminance(worldPos, worldDir, lightDir, atmo,
                                                     RTSKY_INTEGRATE_GROUND, 20.0f,
                                                     g_TransmittanceLut, g_MultiScatterLut);
    gs_MultiScatAs1[z] = r.multiScatAs1 * sphereSolidAngle / (sqrtSamples * sqrtSamples);
    gs_L[z] = r.L * sphereSolidAngle / (sqrtSamples * sqrtSamples);
    GroupMemoryBarrierWithGroupSync();

    [unroll]
    for (uint stride = 32; stride > 0; stride >>= 1)
    {
        if (z < stride)
        {
            gs_MultiScatAs1[z] += gs_MultiScatAs1[z + stride];
            gs_L[z] += gs_L[z + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (z != 0)
        return;

    float3 multiScatAs1 = gs_MultiScatAs1[0] * isotropicPhase;
    float3 inScattered = gs_L[0] * isotropicPhase;
    // Infinite series of isotropic scattering orders: 1 / (1 - r)  (Hillaire 2020, eq. 10)
    float3 L = inScattered / max(1.0f - multiScatAs1, 1e-4f);
    g_Output[id.xy] = float4(atmo.MultiScatteringFactor * L, 1.0f);
}

[numthreads(8, 8, 1)]
void SkyViewCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= RTSKY_SKYVIEW_W || id.y >= RTSKY_SKYVIEW_H)
        return;

    AtmosphereParameters atmo = GetAtmosphere();
    float viewHeight = CameraViewHeight();
    float2 uv = (float2(id.xy) + 0.5f) / float2(RTSKY_SKYVIEW_W, RTSKY_SKYVIEW_H);

    float viewZenithCos, lightViewCos;
    UvToSkyViewLutParams(atmo, viewHeight, uv, viewZenithCos, lightViewCos);

    // Light direction in the LUT's local frame: azimuth 0, same zenith angle as the real light.
    float3 lightDirWorld = g_Frame.lightSelect.x > 0.5f ? g_Frame.sunDir.xyz : g_Frame.moonDir.xyz;
    float lightZenithCos = clamp(lightDirWorld.z, -1.0f, 1.0f);
    float3 lightDir = normalize(float3(sqrt(saturate(1.0f - lightZenithCos * lightZenithCos)), 0.0f, lightZenithCos));

    float viewZenithSin = sqrt(saturate(1.0f - viewZenithCos * viewZenithCos));
    float3 worldDir = float3(viewZenithSin * lightViewCos,
                             viewZenithSin * sqrt(saturate(1.0f - lightViewCos * lightViewCos)),
                             viewZenithCos);
    float3 worldPos = float3(0.0f, 0.0f, viewHeight);

    ScatteringResult r = IntegrateScatteredLuminance(worldPos, worldDir, lightDir, atmo,
                                                     RTSKY_INTEGRATE_VARIABLE_SPP | RTSKY_INTEGRATE_PHASE | RTSKY_INTEGRATE_MULTISCAT,
                                                     30.0f, g_TransmittanceLut, g_MultiScatterLut);
    g_Output[id.xy] = float4(r.L, 1.0f);
}
