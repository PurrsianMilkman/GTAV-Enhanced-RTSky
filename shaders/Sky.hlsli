// RTSky - sky radiance model used by the trace, SH projection and composite passes
#ifndef RTSKY_SKY_HLSLI
#define RTSKY_SKY_HLSLI

#include "Atmosphere.hlsli"

// Direction towards the light that currently illuminates the sky (sun by day, moon by night).
float3 SkyLightDirection()
{
    return g_Frame.lightSelect.x > 0.5f ? g_Frame.sunDir.xyz : g_Frame.moonDir.xyz;
}

float3 SkyLightIlluminance()
{
    return g_Frame.lightSelect.x > 0.5f ? g_Frame.solarIlluminance.rgb : g_Frame.moonIlluminance.rgb;
}

// The trace stores S and U relative to the luminance of the light that illuminates the sky, so they
// stay well inside the half-float range by day and by night (moonlit sky radiance is ~1e-8 in
// absolute units, below the smallest fp16 subnormal). R = S / U is unaffected by the scale.
float TraceRadianceScale()
{
    return 1.0f / max(Luminance(SkyLightIlluminance()), 1e-12f);
}

// Clear-sky luminance for unit light illuminance, looked up in the sky-view LUT.
// dir: unit world direction (Z up).
float3 SkyViewLuminance(Texture2D<float4> skyViewLut, float3 dir)
{
    AtmosphereParameters atmo = GetAtmosphere();
    float viewHeight = CameraViewHeight();
    float3 lightDir = SkyLightDirection();

    float2 dxy = dir.xy;
    float2 lxy = lightDir.xy;
    float lenD = length(dxy);
    float lenL = length(lxy);
    float lightViewCos = (lenD > 1e-5f && lenL > 1e-5f) ? dot(dxy, lxy) / (lenD * lenL) : 1.0f;

    float3 camPos = float3(0.0f, 0.0f, viewHeight);
    bool intersectGround = RaySphereIntersectNearest(camPos, dir, float3(0.0f, 0.0f, 0.0f), atmo.BottomRadius) >= 0.0f;
    float2 uv = SkyViewLutParamsToUv(atmo, intersectGround, dir.z, lightViewCos, viewHeight);
    return skyViewLut.SampleLevel(g_LinearClamp, uv, 0).rgb;
}

// Normalised CIE overcast sky shape (1 + 2 cos(theta)) / 3, theta from the zenith.
float OvercastShape(float cosZenith)
{
    return (1.0f + 2.0f * cosZenith) * (1.0f / 3.0f);
}

// Sky radiance towards dir (upper hemisphere only; ground-bounce is left to the game's own GI).
// overcastZenith: SkyData.overcast (rgb = L_z, w = cloudiness).
float3 SkyRadiance(Texture2D<float4> skyViewLut, float3 dir, float4 overcastZenith)
{
    if (dir.z <= 0.0f)
        return 0.0f;
    float3 clearSky = SkyViewLuminance(skyViewLut, dir) * SkyLightIlluminance();
    float c = overcastZenith.w;
    return lerp(clearSky, overcastZenith.rgb * OvercastShape(dir.z), c);
}

// Irradiance of the unoccluded upper-hemisphere sky on a surface with normal n.
float3 EvaluateSkyIrradianceSH(StructuredBuffer<SkyData> skyData, float3 n)
{
    float y[9];
    SHBasis9(n, y);
    float3 e = 0.0f;
    [unroll]
    for (uint i = 0; i < 9; ++i)
        e += skyData[0].sh[i].rgb * y[i];
    return max(e, 0.0f);
}

#endif // RTSKY_SKY_HLSLI
