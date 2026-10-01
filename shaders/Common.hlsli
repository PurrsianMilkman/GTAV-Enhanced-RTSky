// RTSky - common shader code (bindings, camera reconstruction, sampling, packing)
#ifndef RTSKY_COMMON_HLSLI
#define RTSKY_COMMON_HLSLI

#include "RTSkyShared.h"

static const float PI = 3.14159265358979f;
static const float TWO_PI = 6.28318530717959f;
static const float INV_PI = 0.31830988618379f;

ConstantBuffer<FrameConstants> g_Frame : register(b0);
ConstantBuffer<PassConstants> g_Pass : register(b1);
SamplerState g_LinearClamp : register(s0);
SamplerState g_PointClamp : register(s1);

// ---------------------------------------------------------------------------------------------
// Packing
// ---------------------------------------------------------------------------------------------
float2 OctWrap(float2 v)
{
    return (1.0f - abs(v.yx)) * select(v.xy >= 0.0f, 1.0f, -1.0f);
}

// Unit vector -> [-1,1]^2 (octahedral)
float2 EncodeNormalOct(float3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    n.xy = n.z >= 0.0f ? n.xy : OctWrap(n.xy);
    return n.xy;
}

float3 DecodeNormalOct(float2 e)
{
    float3 n = float3(e.x, e.y, 1.0f - abs(e.x) - abs(e.y));
    float t = saturate(-n.z);
    n.xy += select(n.xy >= 0.0f, -t, t);
    return normalize(n);
}

float Luminance(float3 c)
{
    return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

// ---------------------------------------------------------------------------------------------
// Camera / depth
// ---------------------------------------------------------------------------------------------
// Device depth -> view-space distance along the forward axis (metres). Returns 0 for "no geometry".
float DeviceDepthToViewZ(float d, float nearZ, float farZ, uint mode)
{
    if (mode == RTSKY_DEPTH_STANDARD)
    {
        if (d >= 1.0f)
            return 0.0f;
        return nearZ * farZ / (farZ - d * (farZ - nearZ));
    }
    if (d <= 0.0f)
        return 0.0f;
    if (mode == RTSKY_DEPTH_REVERSED_INFINITE)
        return nearZ / d;
    return nearZ * farZ / (d * (farZ - nearZ) + nearZ);
}

// Unnormalised camera ray (world axes) through normalised screen position uv (0..1, y down),
// scaled so that position = ray * viewZ.
float3 CameraRay(float2 uv, float3 right, float3 up, float3 forward)
{
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    return forward + ndc.x * right + ndc.y * up;
}

float3 CameraRayCurrent(float2 uv)
{
    return CameraRay(uv, g_Frame.camRight.xyz, g_Frame.camUp.xyz, g_Frame.camForward.xyz);
}

// Camera-relative position -> uv of the previous frame. Returns false if behind the camera.
bool ProjectPrevious(float3 posRel, out float2 uv, out float prevViewZ)
{
    float3 p = posRel - g_Frame.prevCamOffset.xyz;
    float3 fwd = g_Frame.prevCamForward.xyz;
    float3 right = g_Frame.prevCamRight.xyz;
    float3 up = g_Frame.prevCamUp.xyz;
    prevViewZ = dot(p, fwd);
    uv = 0.0f;
    if (prevViewZ <= 1e-3f)
        return false;
    // right/up vectors carry tan(fov/2); project onto them normalised by their squared length.
    float x = dot(p, right) / (dot(right, right) * prevViewZ);
    float y = dot(p, up) / (dot(up, up) * prevViewZ);
    uv = float2(x * 0.5f + 0.5f, 0.5f - y * 0.5f);
    return true;
}

float2 PixelToUV(uint2 pixel)
{
    return (float2(pixel) + 0.5f) * g_Frame.traceSize.zw;
}

// ---------------------------------------------------------------------------------------------
// Random numbers / low-discrepancy sampling
// ---------------------------------------------------------------------------------------------
uint PcgHash(uint v)
{
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float UintToUnitFloat(uint v)
{
    return float(v >> 8) * (1.0f / 16777216.0f);
}

// Interleaved gradient noise (Jimenez 2014), per pixel, animated per frame
float InterleavedGradientNoise(float2 pixel, uint frame)
{
    pixel += 5.588238f * float(frame & 63u);
    return frac(52.9829189f * frac(0.06711056f * pixel.x + 0.00583715f * pixel.y));
}

// 2D sample i of a per-pixel Cranley-Patterson rotated R2 sequence. The sequence step n * alpha is
// done in 0.32 fixed point (exact modulo 1 for any n): in float32, alpha * n loses every fractional
// bit once the frame counter is large, and all rays would collapse onto a few directions.
float2 SampleR2(uint2 pixel, uint frame, uint i)
{
    const uint2 alpha = uint2(3242174889u, 2447445414u); // (0.7548776662, 0.5698402910) * 2^32
    float2 rotation = float2(InterleavedGradientNoise(float2(pixel), frame),
                             UintToUnitFloat(PcgHash(pixel.x * 1973u + pixel.y * 9277u + frame * 26699u)));
    uint2 q = (frame * 16u + i) * alpha;
    return frac(rotation + float2(UintToUnitFloat(q.x), UintToUnitFloat(q.y)));
}

void BuildBasis(float3 n, out float3 t, out float3 b)
{
    // Duff et al. 2017, "Building an Orthonormal Basis, Revisited"
    float s = n.z >= 0.0f ? 1.0f : -1.0f;
    float a = -1.0f / (s + n.z);
    float c = n.x * n.y * a;
    t = float3(1.0f + s * n.x * n.x * a, s * c, -s * n.x);
    b = float3(c, s + n.y * n.y * a, -n.y);
}

float3 CosineSampleHemisphere(float3 n, float2 u)
{
    float r = sqrt(u.x);
    float phi = TWO_PI * u.y;
    float3 t, b;
    BuildBasis(n, t, b);
    float3 d = t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(max(0.0f, 1.0f - u.x));
    return normalize(d);
}

// Uniform direction inside a cone of half-angle cosThetaMax around axis
float3 SampleCone(float3 axis, float cosThetaMax, float2 u)
{
    float cosTheta = lerp(1.0f, cosThetaMax, u.x);
    float sinTheta = sqrt(saturate(1.0f - cosTheta * cosTheta));
    float phi = TWO_PI * u.y;
    float3 t, b;
    BuildBasis(axis, t, b);
    return normalize(t * (sinTheta * cos(phi)) + b * (sinTheta * sin(phi)) + axis * cosTheta);
}

// ---------------------------------------------------------------------------------------------
// Spherical harmonics (real, L2) - Ramamoorthi & Hanrahan 2001 basis ordering
// ---------------------------------------------------------------------------------------------
void SHBasis9(float3 n, out float y[9])
{
    y[0] = 0.282095f;
    y[1] = 0.488603f * n.y;
    y[2] = 0.488603f * n.z;
    y[3] = 0.488603f * n.x;
    y[4] = 1.092548f * n.x * n.y;
    y[5] = 1.092548f * n.y * n.z;
    y[6] = 0.315392f * (3.0f * n.z * n.z - 1.0f);
    y[7] = 1.092548f * n.x * n.z;
    y[8] = 0.546274f * (n.x * n.x - n.y * n.y);
}

#endif // RTSKY_COMMON_HLSLI
