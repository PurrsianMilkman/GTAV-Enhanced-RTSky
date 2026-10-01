// RTSky - sky irradiance projection
// One group of 64 threads. Evaluates the clear sky on 1024 Fibonacci directions of the upper
// hemisphere, projects it (and the CIE overcast shape) onto real SH L2, blends by cloudiness,
// convolves with the clamped cosine and writes SkyData. Also computes sun / moon irradiance at the
// camera through the transmittance LUT.
//   SRV t0 = SkyViewLut, t1 = TransmittanceLut      UAV u0 = RWStructuredBuffer<SkyData>
#include "Sky.hlsli"

Texture2D<float4> g_SkyViewLut : register(t0);
Texture2D<float4> g_TransmittanceLut : register(t1);
RWStructuredBuffer<SkyData> g_SkyData : register(u0);

groupshared float3 gs_ShClear[RTSKY_SKYPROJECT_THREADS][9];
groupshared float gs_ShShape[RTSKY_SKYPROJECT_THREADS][9];
groupshared float3 gs_HorizClear[RTSKY_SKYPROJECT_THREADS];

float3 FibonacciHemisphere(uint i, uint n)
{
    // z uniformly distributed in (0,1] -> uniform over the hemisphere's solid angle
    const float goldenAngle = 2.39996322972865f;
    float z = 1.0f - (float(i) + 0.5f) / float(n);
    float r = sqrt(saturate(1.0f - z * z));
    float phi = goldenAngle * float(i);
    return float3(r * cos(phi), r * sin(phi), z);
}

// Light irradiance (on a surface facing the light) after the atmosphere, 0 below the horizon.
float3 LightIrradianceAtCamera(float3 lightDir, float3 illuminance, float angularRadius)
{
    AtmosphereParameters atmo = GetAtmosphere();
    float viewHeight = CameraViewHeight();
    float3 camPos = float3(0.0f, 0.0f, viewHeight);
    // Fraction of the disc above the planet's horizon (smooth over the disc diameter)
    float horizonCos = -sqrt(max(viewHeight * viewHeight - atmo.BottomRadius * atmo.BottomRadius, 0.0f)) / viewHeight;
    float sinRadius = sin(max(angularRadius, 0.0047f));
    float visible = saturate((lightDir.z - horizonCos) / (2.0f * sinRadius) + 0.5f);
    float3 t = SampleTransmittance(g_TransmittanceLut, atmo, viewHeight, max(lightDir.z, horizonCos + 1e-4f));
    return illuminance * t * visible;
}

[numthreads(RTSKY_SKYPROJECT_THREADS, 1, 1)]
void SkyProjectCS(uint tid : SV_GroupIndex)
{
    const uint dirsPerThread = RTSKY_SH_DIRECTIONS / RTSKY_SKYPROJECT_THREADS;
    const float sampleWeight = TWO_PI / float(RTSKY_SH_DIRECTIONS); // uniform hemisphere measure

    float3 shClear[9];
    float shShape[9];
    [unroll]
    for (uint k = 0; k < 9; ++k)
    {
        shClear[k] = 0.0f;
        shShape[k] = 0.0f;
    }
    float3 horizClear = 0.0f;

    for (uint d = 0; d < dirsPerThread; ++d)
    {
        float3 dir = FibonacciHemisphere(tid * dirsPerThread + d, RTSKY_SH_DIRECTIONS);
        float3 L = SkyViewLuminance(g_SkyViewLut, dir) * SkyLightIlluminance();
        float shape = OvercastShape(dir.z);
        float y[9];
        SHBasis9(dir, y);
        [unroll]
        for (uint k2 = 0; k2 < 9; ++k2)
        {
            shClear[k2] += L * (y[k2] * sampleWeight);
            shShape[k2] += shape * y[k2] * sampleWeight;
        }
        horizClear += L * (dir.z * sampleWeight);
    }

    [unroll]
    for (uint k3 = 0; k3 < 9; ++k3)
    {
        gs_ShClear[tid][k3] = shClear[k3];
        gs_ShShape[tid][k3] = shShape[k3];
    }
    gs_HorizClear[tid] = horizClear;
    GroupMemoryBarrierWithGroupSync();

    for (uint stride = RTSKY_SKYPROJECT_THREADS / 2; stride > 0; stride >>= 1)
    {
        if (tid < stride)
        {
            [unroll]
            for (uint k4 = 0; k4 < 9; ++k4)
            {
                gs_ShClear[tid][k4] += gs_ShClear[tid + stride][k4];
                gs_ShShape[tid][k4] += gs_ShShape[tid + stride][k4];
            }
            gs_HorizClear[tid] += gs_HorizClear[tid + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (tid != 0)
        return;

    // Sun and moon irradiance at the camera (clear atmosphere), then weather attenuation.
    float3 sunIrr = LightIrradianceAtCamera(g_Frame.sunDir.xyz, g_Frame.solarIlluminance.rgb, 0.0047f);
    float3 moonIrr = LightIrradianceAtCamera(g_Frame.moonDir.xyz, g_Frame.moonIlluminance.rgb, 0.0045f);

    float c = saturate(g_Frame.weather.x);
    float overcastTransmission = g_Frame.weather.y;
    float directFactor = g_Frame.weather.z;

    // Overcast sky: diffuse transmission of everything that would have reached a horizontal surface.
    float3 lightHoriz = (g_Frame.lightSelect.x > 0.5f)
        ? sunIrr * max(g_Frame.sunDir.z, 0.0f)
        : moonIrr * max(g_Frame.moonDir.z, 0.0f);
    float3 clearHoriz = gs_HorizClear[0];
    float targetLum = overcastTransmission * Luminance(lightHoriz + clearHoriz);
    // Horizontal irradiance of L_z * (1 + 2 cos) / 3 over the hemisphere is L_z * 7*pi/9.
    float lz = targetLum / (7.0f * PI / 9.0f);
    // Slightly tint the overcast sky by the clear sky's hue (keeps sunsets warm under thin cloud).
    float3 clearHue = clearHoriz / max(Luminance(clearHoriz), 1e-8f);
    float3 overcastZenith = lz * lerp(float3(1.0f, 1.0f, 1.0f), clearHue, 0.25f);

    // Cosine convolution (Ramamoorthi & Hanrahan 2001): A0 = pi, A1 = 2pi/3, A2 = pi/4
    const float A[9] = { PI, 2.0f * PI / 3.0f, 2.0f * PI / 3.0f, 2.0f * PI / 3.0f,
                         PI / 4.0f, PI / 4.0f, PI / 4.0f, PI / 4.0f, PI / 4.0f };

    SkyData outData;
    [unroll]
    for (uint k5 = 0; k5 < 9; ++k5)
    {
        float3 radianceSH = lerp(gs_ShClear[0][k5], overcastZenith * gs_ShShape[0][k5], c);
        outData.sh[k5] = float4(radianceSH * A[k5], 0.0f);
    }
    outData.sunIrradiance = float4(sunIrr * directFactor, 0.0f);
    outData.moonIrradiance = float4(moonIrr * directFactor, 0.0f);
    float3 skyHoriz = lerp(clearHoriz, overcastZenith * (7.0f * PI / 9.0f), c);
    outData.skyHorizontal = float4(skyHoriz, Luminance(clearHoriz));
    outData.overcast = float4(overcastZenith, c);
    g_SkyData[0] = outData;
}
