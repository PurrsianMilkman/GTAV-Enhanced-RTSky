// RTSky - physically based atmosphere
// Implementation of S. Hillaire, "A Scalable and Production Ready Sky and Atmosphere Rendering
// Technique", EGSR 2020 (reference: github.com/sebh/UnrealEngineSkyAtmosphere, MIT licence).
// Units: kilometres. Planet centre at the origin, +Z up at the camera.
#ifndef RTSKY_ATMOSPHERE_HLSLI
#define RTSKY_ATMOSPHERE_HLSLI

#include "Common.hlsli"

static const float PLANET_RADIUS_OFFSET = 0.01f; // km, keeps sample positions off the ground sphere

struct AtmosphereParameters
{
    float BottomRadius;
    float TopRadius;
    float RayleighDensityExpScale;
    float3 RayleighScattering;
    float MieDensityExpScale;
    float MieScattering;
    float MieExtinction;
    float MieAbsorption;
    float MiePhaseG;
    float OzoneCentre;
    float OzoneHalfWidth;
    float3 OzoneAbsorption;
    float3 GroundAlbedo;
    float MultiScatteringFactor;
};

AtmosphereParameters GetAtmosphere()
{
    AtmosphereParameters a;
    a.BottomRadius = g_Frame.atmoRadii.x;
    a.TopRadius = g_Frame.atmoRadii.y;
    a.RayleighScattering = g_Frame.rayleighScattering.rgb;
    a.RayleighDensityExpScale = -1.0f / max(g_Frame.rayleighScattering.w, 1e-3f);
    a.MieScattering = g_Frame.mieParams.x;
    a.MieExtinction = g_Frame.mieParams.y;
    a.MieAbsorption = g_Frame.mieParams.z;
    a.MieDensityExpScale = -1.0f / max(g_Frame.mieParams.w, 1e-3f);
    a.MiePhaseG = g_Frame.atmoRadii.w;
    a.OzoneAbsorption = g_Frame.ozoneAbsorption.rgb;
    a.OzoneCentre = g_Frame.ozoneAbsorption.w;
    a.OzoneHalfWidth = max(0.5f * g_Frame.atmoMisc.x, 1e-3f);
    a.GroundAlbedo = g_Frame.atmoMisc.yyy;
    a.MultiScatteringFactor = g_Frame.atmoMisc.z;
    return a;
}

// Camera distance from the planet centre (km)
float CameraViewHeight()
{
    return g_Frame.atmoRadii.x + max(g_Frame.atmoRadii.z, PLANET_RADIUS_OFFSET);
}

// ---------------------------------------------------------------------------------------------
// Geometry helpers
// ---------------------------------------------------------------------------------------------
// Nearest non-negative intersection distance with a sphere, or -1.
float RaySphereIntersectNearest(float3 r0, float3 rd, float3 s0, float sR)
{
    float a = dot(rd, rd);
    float3 s0r0 = r0 - s0;
    float b = 2.0f * dot(rd, s0r0);
    float c = dot(s0r0, s0r0) - sR * sR;
    float delta = b * b - 4.0f * a * c;
    if (delta < 0.0f || a == 0.0f)
        return -1.0f;
    float sq = sqrt(delta);
    float sol0 = (-b - sq) / (2.0f * a);
    float sol1 = (-b + sq) / (2.0f * a);
    if (sol0 < 0.0f && sol1 < 0.0f)
        return -1.0f;
    if (sol0 < 0.0f)
        return max(0.0f, sol1);
    if (sol1 < 0.0f)
        return max(0.0f, sol0);
    return max(0.0f, min(sol0, sol1));
}

float FromUnitToSubUvs(float u, float resolution)
{
    return (u + 0.5f / resolution) * (resolution / (resolution + 1.0f));
}

float FromSubUvsToUnit(float u, float resolution)
{
    return (u - 0.5f / resolution) * (resolution / (resolution - 1.0f));
}

// ---------------------------------------------------------------------------------------------
// Participating media
// ---------------------------------------------------------------------------------------------
struct MediumSample
{
    float3 scatteringRay;
    float3 scatteringMie;
    float3 scattering;
    float3 extinction;
};

MediumSample SampleMedium(float3 worldPos, AtmosphereParameters atmo)
{
    float h = length(worldPos) - atmo.BottomRadius;
    float densityMie = exp(atmo.MieDensityExpScale * h);
    float densityRay = exp(atmo.RayleighDensityExpScale * h);
    // Ozone: tent profile, 1 at the centre altitude, 0 at centre +/- half width
    float densityOzo = saturate(1.0f - abs(h - atmo.OzoneCentre) / atmo.OzoneHalfWidth);

    MediumSample s;
    s.scatteringMie = densityMie * atmo.MieScattering.xxx;
    s.scatteringRay = densityRay * atmo.RayleighScattering;
    s.scattering = s.scatteringMie + s.scatteringRay;
    s.extinction = densityMie * atmo.MieExtinction.xxx + s.scatteringRay + densityOzo * atmo.OzoneAbsorption;
    return s;
}

float RayleighPhase(float cosTheta)
{
    return 3.0f / (16.0f * PI) * (1.0f + cosTheta * cosTheta);
}

// Cornette-Shanks phase function; cosTheta = dot(light direction, view direction)
float MiePhase(float g, float cosTheta)
{
    float k = 3.0f / (8.0f * PI) * (1.0f - g * g) / (2.0f + g * g);
    return k * (1.0f + cosTheta * cosTheta) / pow(max(1.0f + g * g - 2.0f * g * cosTheta, 1e-4f), 1.5f);
}

// ---------------------------------------------------------------------------------------------
// LUT parameterisations
// ---------------------------------------------------------------------------------------------
void UvToTransmittanceLutParams(AtmosphereParameters atmo, float2 uv, out float viewHeight, out float viewZenithCos)
{
    float xMu = uv.x;
    float xR = uv.y;
    float H = sqrt(atmo.TopRadius * atmo.TopRadius - atmo.BottomRadius * atmo.BottomRadius);
    float rho = H * xR;
    viewHeight = sqrt(rho * rho + atmo.BottomRadius * atmo.BottomRadius);
    float dMin = atmo.TopRadius - viewHeight;
    float dMax = rho + H;
    float d = dMin + xMu * (dMax - dMin);
    viewZenithCos = d == 0.0f ? 1.0f : (H * H - rho * rho - d * d) / (2.0f * viewHeight * d);
    viewZenithCos = clamp(viewZenithCos, -1.0f, 1.0f);
}

float2 TransmittanceLutParamsToUv(AtmosphereParameters atmo, float viewHeight, float viewZenithCos)
{
    float H = sqrt(max(0.0f, atmo.TopRadius * atmo.TopRadius - atmo.BottomRadius * atmo.BottomRadius));
    float rho = sqrt(max(0.0f, viewHeight * viewHeight - atmo.BottomRadius * atmo.BottomRadius));
    float discriminant = viewHeight * viewHeight * (viewZenithCos * viewZenithCos - 1.0f) + atmo.TopRadius * atmo.TopRadius;
    float d = max(0.0f, -viewHeight * viewZenithCos + sqrt(max(discriminant, 0.0f)));
    float dMin = atmo.TopRadius - viewHeight;
    float dMax = rho + H;
    float xMu = (d - dMin) / max(dMax - dMin, 1e-6f);
    float xR = rho / max(H, 1e-6f);
    return float2(xMu, xR);
}

// Sky-view LUT with the non-linear latitude mapping of the reference implementation.
void UvToSkyViewLutParams(AtmosphereParameters atmo, float viewHeight, float2 uv, out float viewZenithCos, out float lightViewCos)
{
    uv = float2(FromSubUvsToUnit(uv.x, RTSKY_SKYVIEW_W), FromSubUvsToUnit(uv.y, RTSKY_SKYVIEW_H));

    float vHorizon = sqrt(max(viewHeight * viewHeight - atmo.BottomRadius * atmo.BottomRadius, 0.0f));
    float cosBeta = vHorizon / viewHeight;
    float beta = acos(cosBeta);
    float zenithHorizonAngle = PI - beta;

    if (uv.y < 0.5f)
    {
        float coord = 2.0f * uv.y;
        coord = 1.0f - coord;
        coord *= coord;
        coord = 1.0f - coord;
        viewZenithCos = cos(zenithHorizonAngle * coord);
    }
    else
    {
        float coord = uv.y * 2.0f - 1.0f;
        coord *= coord;
        viewZenithCos = cos(zenithHorizonAngle + beta * coord);
    }

    float coordX = uv.x * uv.x;
    lightViewCos = -(coordX * 2.0f - 1.0f);
}

float2 SkyViewLutParamsToUv(AtmosphereParameters atmo, bool intersectGround, float viewZenithCos, float lightViewCos, float viewHeight)
{
    float vHorizon = sqrt(max(viewHeight * viewHeight - atmo.BottomRadius * atmo.BottomRadius, 0.0f));
    float cosBeta = vHorizon / viewHeight;
    float beta = acos(cosBeta);
    float zenithHorizonAngle = PI - beta;

    float2 uv;
    if (!intersectGround)
    {
        float coord = acos(clamp(viewZenithCos, -1.0f, 1.0f)) / zenithHorizonAngle;
        coord = 1.0f - coord;
        coord = sqrt(saturate(coord));
        coord = 1.0f - coord;
        uv.y = coord * 0.5f;
    }
    else
    {
        float coord = (acos(clamp(viewZenithCos, -1.0f, 1.0f)) - zenithHorizonAngle) / beta;
        coord = sqrt(saturate(coord));
        uv.y = coord * 0.5f + 0.5f;
    }

    uv.x = sqrt(saturate(-lightViewCos * 0.5f + 0.5f));
    return float2(FromUnitToSubUvs(uv.x, RTSKY_SKYVIEW_W), FromUnitToSubUvs(uv.y, RTSKY_SKYVIEW_H));
}

float3 SampleTransmittance(Texture2D<float4> lut, AtmosphereParameters atmo, float viewHeight, float viewZenithCos)
{
    float2 uv = TransmittanceLutParamsToUv(atmo, viewHeight, viewZenithCos);
    return lut.SampleLevel(g_LinearClamp, uv, 0).rgb;
}

float3 SampleMultipleScattering(Texture2D<float4> lut, AtmosphereParameters atmo, float3 worldPos, float viewZenithCos)
{
    float2 uv = saturate(float2(viewZenithCos * 0.5f + 0.5f,
                                (length(worldPos) - atmo.BottomRadius) / (atmo.TopRadius - atmo.BottomRadius)));
    uv = float2(FromUnitToSubUvs(uv.x, RTSKY_MULTISCATTER_SIZE), FromUnitToSubUvs(uv.y, RTSKY_MULTISCATTER_SIZE));
    return lut.SampleLevel(g_LinearClamp, uv, 0).rgb;
}

// ---------------------------------------------------------------------------------------------
// Ray-marched single scattering (+ multiple scattering approximation from the Psi_ms LUT)
// ---------------------------------------------------------------------------------------------
struct ScatteringResult
{
    float3 L;              // in-scattered luminance for unit light illuminance
    float3 opticalDepth;
    float3 multiScatAs1;   // f_ms integrand (only meaningful with isotropic phase)
};

// mode flags
#define RTSKY_INTEGRATE_GROUND       0x1   // add light bounced off the ground (multi-scattering LUT)
#define RTSKY_INTEGRATE_VARIABLE_SPP 0x2   // 4..14 samples with quadratic distribution (sky view)
#define RTSKY_INTEGRATE_PHASE        0x4   // Rayleigh + Mie phase; otherwise isotropic
#define RTSKY_INTEGRATE_MULTISCAT    0x8   // add Psi_ms LUT contribution

ScatteringResult IntegrateScatteredLuminance(float3 worldPos, float3 worldDir, float3 lightDir, AtmosphereParameters atmo,
                                             uint mode, float sampleCountIni,
                                             Texture2D<float4> transmittanceLut, Texture2D<float4> multiScatLut)
{
    ScatteringResult result = (ScatteringResult)0;

    const float3 earthO = float3(0.0f, 0.0f, 0.0f);
    float tBottom = RaySphereIntersectNearest(worldPos, worldDir, earthO, atmo.BottomRadius);
    float tTop = RaySphereIntersectNearest(worldPos, worldDir, earthO, atmo.TopRadius);
    float tMax = 0.0f;
    if (tBottom < 0.0f)
    {
        if (tTop < 0.0f)
            return result;
        tMax = tTop;
    }
    else if (tTop > 0.0f)
    {
        tMax = min(tTop, tBottom);
    }
    tMax = min(tMax, 9000000.0f);

    float sampleCount = sampleCountIni;
    float sampleCountFloor = sampleCountIni;
    float tMaxFloor = tMax;
    if (mode & RTSKY_INTEGRATE_VARIABLE_SPP)
    {
        sampleCount = lerp(4.0f, 14.0f, saturate(tMax * 0.01f));
        sampleCountFloor = floor(sampleCount);
        tMaxFloor = tMax * sampleCountFloor / sampleCount;
    }
    float dt = tMax / sampleCount;

    const float uniformPhase = 1.0f / (4.0f * PI);
    float cosTheta = dot(lightDir, worldDir);
    float miePhaseValue = MiePhase(atmo.MiePhaseG, cosTheta);
    float rayleighPhaseValue = RayleighPhase(cosTheta);

    float3 L = 0.0f;
    float3 throughput = 1.0f;
    float3 opticalDepth = 0.0f;
    float t = 0.0f;
    const float sampleSegmentT = 0.3f;

    for (float s = 0.0f; s < sampleCount; s += 1.0f)
    {
        if (mode & RTSKY_INTEGRATE_VARIABLE_SPP)
        {
            float t0 = s / sampleCountFloor;
            float t1 = (s + 1.0f) / sampleCountFloor;
            t0 = t0 * t0;
            t1 = t1 * t1;
            t0 = tMaxFloor * t0;
            t1 = t1 > 1.0f ? tMax : tMaxFloor * t1;
            t = t0 + (t1 - t0) * sampleSegmentT;
            dt = t1 - t0;
        }
        else
        {
            float newT = tMax * (s + sampleSegmentT) / sampleCount;
            dt = newT - t;
            t = newT;
        }

        float3 P = worldPos + t * worldDir;
        MediumSample medium = SampleMedium(P, atmo);
        float3 sampleOpticalDepth = medium.extinction * dt;
        float3 sampleTransmittance = exp(-sampleOpticalDepth);
        opticalDepth += sampleOpticalDepth;

        {
            float pHeight = length(P);
            float3 upVector = P / pHeight;
            float lightZenithCos = dot(lightDir, upVector);
            float3 transmittanceToLight = SampleTransmittance(transmittanceLut, atmo, pHeight, lightZenithCos);

            float3 phaseTimesScattering = (mode & RTSKY_INTEGRATE_PHASE)
                ? medium.scatteringMie * miePhaseValue + medium.scatteringRay * rayleighPhaseValue
                : medium.scattering * uniformPhase;

            float tEarth = RaySphereIntersectNearest(P, lightDir, earthO + PLANET_RADIUS_OFFSET * upVector, atmo.BottomRadius);
            float earthShadow = tEarth >= 0.0f ? 0.0f : 1.0f;

            float3 multiScattered = 0.0f;
            if (mode & RTSKY_INTEGRATE_MULTISCAT)
                multiScattered = SampleMultipleScattering(multiScatLut, atmo, P, lightZenithCos);

            float3 S = earthShadow * transmittanceToLight * phaseTimesScattering + multiScattered * medium.scattering;

            float3 safeExtinction = max(medium.extinction, 1e-7f);
            float3 MSint = (medium.scattering - medium.scattering * sampleTransmittance) / safeExtinction;
            result.multiScatAs1 += throughput * MSint;
            float3 Sint = (S - S * sampleTransmittance) / safeExtinction;
            L += throughput * Sint;
        }
        throughput *= sampleTransmittance;
    }

    if ((mode & RTSKY_INTEGRATE_GROUND) && tMax == tBottom && tBottom > 0.0f)
    {
        float3 P = worldPos + tBottom * worldDir;
        float pHeight = length(P);
        float3 upVector = P / pHeight;
        float lightZenithCos = dot(lightDir, upVector);
        float3 transmittanceToLight = SampleTransmittance(transmittanceLut, atmo, pHeight, lightZenithCos);
        float NdotL = saturate(dot(upVector, lightDir));
        L += transmittanceToLight * throughput * NdotL * atmo.GroundAlbedo / PI;
    }

    result.L = L;
    result.opticalDepth = opticalDepth;
    return result;
}

// Optical depth from worldPos along worldDir to the top of the atmosphere (or the ground), used to
// build the transmittance LUT. 40 uniform samples as in the reference implementation.
float3 IntegrateOpticalDepth(float3 worldPos, float3 worldDir, AtmosphereParameters atmo)
{
    const float3 earthO = float3(0.0f, 0.0f, 0.0f);
    float tBottom = RaySphereIntersectNearest(worldPos, worldDir, earthO, atmo.BottomRadius);
    float tTop = RaySphereIntersectNearest(worldPos, worldDir, earthO, atmo.TopRadius);
    float tMax = 0.0f;
    if (tBottom < 0.0f)
    {
        if (tTop < 0.0f)
            return 0.0f;
        tMax = tTop;
    }
    else if (tTop > 0.0f)
    {
        tMax = min(tTop, tBottom);
    }

    const float sampleCount = 40.0f;
    float3 opticalDepth = 0.0f;
    float t = 0.0f;
    for (float s = 0.0f; s < sampleCount; s += 1.0f)
    {
        float newT = tMax * (s + 0.3f) / sampleCount;
        float dt = newT - t;
        t = newT;
        MediumSample medium = SampleMedium(worldPos + t * worldDir, atmo);
        opticalDepth += medium.extinction * dt;
    }
    return opticalDepth;
}

#endif // RTSKY_ATMOSPHERE_HLSLI
