// RTSky - prepare pass: game depth -> linear view depth + reconstructed world normal
//   SRV t0 = game depth (plane 0 viewed as R32_FLOAT)   UAV u0 = LinearDepth[cur], u1 = Normal[cur]
//   Dispatch ceil(W/8) x ceil(H/8)
#include "Common.hlsli"

Texture2D<float> g_GameDepth : register(t0);
RWTexture2D<float> g_LinearDepth : register(u0);
RWTexture2D<float2> g_Normal : register(u1);

float LoadViewZ(int2 p)
{
    int2 size = int2(g_Frame.traceSize.xy);
    if (any(p < 0) || any(p >= size))
        return 0.0f;
    int2 texel = p + int2(g_Frame.depthViewport.xy);
    float d = g_GameDepth.Load(int3(texel, 0));
    return DeviceDepthToViewZ(d, g_Frame.camRight.w, g_Frame.camUp.w, (uint)g_Frame.camForward.w);
}

float3 ViewPos(int2 p, float viewZ)
{
    float2 uv = (float2(p) + 0.5f) * g_Frame.traceSize.zw;
    return CameraRayCurrent(uv) * viewZ;
}

// Pick the neighbour with the smaller depth discontinuity (relative to the extrapolated depth).
float3 AxisDerivative(int2 p, int2 axis, float zC, float3 posC)
{
    float zN = LoadViewZ(p - axis);
    float zP = LoadViewZ(p + axis);
    bool validN = zN > 0.0f;
    bool validP = zP > 0.0f;
    if (!validN && !validP)
        return 0.0f;
    float errN = validN ? abs(zN - zC) : 1e30f;
    float errP = validP ? abs(zP - zC) : 1e30f;
    if (errP <= errN)
        return ViewPos(p + axis, zP) - posC;
    return posC - ViewPos(p - axis, zN);
}

[numthreads(RTSKY_GROUP_SIZE, RTSKY_GROUP_SIZE, 1)]
void PrepareCS(uint3 id : SV_DispatchThreadID)
{
    int2 p = int2(id.xy);
    if (any(p >= int2(g_Frame.traceSize.xy)))
        return;

    float zC = LoadViewZ(p);
    if (zC <= 0.0f || zC >= g_Frame.camUp.w * 0.999f)
    {
        g_LinearDepth[p] = 0.0f;
        g_Normal[p] = float2(0.0f, 0.0f);
        return;
    }

    float3 posC = ViewPos(p, zC);
    float3 ddx = AxisDerivative(p, int2(1, 0), zC, posC);
    float3 ddy = AxisDerivative(p, int2(0, 1), zC, posC);
    float3 n = cross(ddx, ddy);
    float len2 = dot(n, n);
    float3 viewDir = normalize(posC);
    if (len2 < 1e-20f)
        n = -viewDir;
    else
        n *= rsqrt(len2);
    // Face the camera
    if (dot(n, viewDir) > 0.0f)
        n = -n;

    g_LinearDepth[p] = zC;
    g_Normal[p] = EncodeNormalOct(n);
}
