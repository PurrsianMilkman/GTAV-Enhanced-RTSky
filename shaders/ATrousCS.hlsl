// RTSky - edge-avoiding a-trous wavelet filter (SVGF-style, Schied et al. 2017)
// S and U are filtered with identical weights so that their ratio stays unbiased by the filter.
//   SRV t0 inS, t1 inU (a = variance), t2 LinearDepth[cur], t3 Normal[cur]
//   UAV u0 outS, u1 outU (a = filtered variance)
//   PassConstants.args0.y = step width (1, 2, 4, 8 ...)
#include "Common.hlsli"

Texture2D<float4> g_InS : register(t0);
Texture2D<float4> g_InU : register(t1);
Texture2D<float> g_Depth : register(t2);
Texture2D<float2> g_Normal : register(t3);
RWTexture2D<float4> g_OutS : register(u0);
RWTexture2D<float4> g_OutU : register(u1);

float GaussianVariance3x3(int2 p)
{
    const float k[3] = { 0.25f, 0.5f, 0.25f };
    float sum = 0.0f;
    float wsum = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            int2 q = clamp(p + int2(x, y), int2(0, 0), int2(g_Frame.traceSize.xy) - 1);
            float w = k[x + 1] * k[y + 1];
            sum += g_InU.Load(int3(q, 0)).a * w;
            wsum += w;
        }
    }
    return sum / wsum;
}

[numthreads(RTSKY_GROUP_SIZE, RTSKY_GROUP_SIZE, 1)]
void ATrousCS(uint3 id : SV_DispatchThreadID)
{
    int2 p = int2(id.xy);
    int2 size = int2(g_Frame.traceSize.xy);
    if (any(p >= size))
        return;

    float4 cS = g_InS.Load(int3(p, 0));
    float4 cU = g_InU.Load(int3(p, 0));
    float zP = g_Depth.Load(int3(p, 0));
    if (zP <= 0.0f)
    {
        g_OutS[p] = cS;
        g_OutU[p] = cU;
        return;
    }

    int step = int(max(g_Pass.args0.y, 1u));
    float3 nP = DecodeNormalOct(g_Normal.Load(int3(p, 0)));
    float3 posP = CameraRayCurrent(PixelToUV(uint2(p))) * zP;
    float lumP = Luminance(cS.rgb);
    float sigmaPlane = g_Frame.denoiseParams.x;
    float normalPower = g_Frame.denoiseParams.y;
    float sigmaL = g_Frame.denoiseParams.z;
    float lumDenom = sigmaL * sqrt(max(GaussianVariance3x3(p), 0.0f)) + 1e-4f;
    float planeDenom = max(sigmaPlane * zP * float(step), 1e-4f);

    const float kernel[3] = { 3.0f / 8.0f, 1.0f / 4.0f, 1.0f / 16.0f };

    float4 sumS = 0.0f;
    float3 sumU = 0.0f;
    float sumVar = 0.0f;
    float sumW = 0.0f;

    [unroll]
    for (int y = -2; y <= 2; ++y)
    {
        [unroll]
        for (int x = -2; x <= 2; ++x)
        {
            int2 q = p + int2(x, y) * step;
            if (any(q < 0) || any(q >= size))
                continue;
            float zQ = g_Depth.Load(int3(q, 0));
            if (zQ <= 0.0f)
                continue;

            float4 sQ = g_InS.Load(int3(q, 0));
            float4 uQ = g_InU.Load(int3(q, 0));
            float3 nQ = DecodeNormalOct(g_Normal.Load(int3(q, 0)));
            float3 posQ = CameraRayCurrent(PixelToUV(uint2(q))) * zQ;

            float wKernel = kernel[abs(x)] * kernel[abs(y)];
            float wPlane = exp(-abs(dot(nP, posQ - posP)) / planeDenom);
            float wNormal = pow(saturate(dot(nP, nQ)), normalPower);
            float wLum = exp(-abs(Luminance(sQ.rgb) - lumP) / lumDenom);
            float w = wKernel * wPlane * wNormal * wLum;

            sumS += sQ * w;
            sumU += uQ.rgb * w;
            sumVar += uQ.a * w * w;
            sumW += w;
        }
    }

    if (sumW > 1e-6f)
    {
        g_OutS[p] = sumS / sumW;
        g_OutU[p] = float4(sumU / sumW, sumVar / (sumW * sumW));
    }
    else
    {
        g_OutS[p] = cS;
        g_OutU[p] = cU;
    }
}
