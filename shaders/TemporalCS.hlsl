// RTSky - temporal accumulation with world-space reprojection
//   SRV t0 TraceS, t1 TraceU, t2 LinearDepth[cur], t3 Normal[cur], t4 LinearDepth[prev], t5 Normal[prev],
//       t6 HistS[prev], t7 HistU[prev], t8 HistMeta[prev]
//   UAV u0 HistS[cur], u1 HistU[cur], u2 HistMeta[cur]
#include "Common.hlsli"

Texture2D<float4> g_TraceS : register(t0);
Texture2D<float4> g_TraceU : register(t1);
Texture2D<float> g_Depth : register(t2);
Texture2D<float2> g_Normal : register(t3);
Texture2D<float> g_PrevDepth : register(t4);
Texture2D<float2> g_PrevNormal : register(t5);
Texture2D<float4> g_PrevHistS : register(t6);
Texture2D<float4> g_PrevHistU : register(t7);
Texture2D<float4> g_PrevHistMeta : register(t8);
RWTexture2D<float4> g_HistS : register(u0);
RWTexture2D<float4> g_HistU : register(u1);
RWTexture2D<float4> g_HistMeta : register(u2);

[numthreads(RTSKY_GROUP_SIZE, RTSKY_GROUP_SIZE, 1)]
void TemporalCS(uint3 id : SV_DispatchThreadID)
{
    uint2 p = id.xy;
    if (any(p >= uint2(g_Frame.traceSize.xy)))
        return;

    float4 curS = g_TraceS.Load(int3(p, 0));
    float4 curU = g_TraceU.Load(int3(p, 0));
    float z = g_Depth.Load(int3(p, 0));

    if (z <= 0.0f || curU.a <= 0.0f)
    {
        g_HistS[p] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        g_HistU[p] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        g_HistMeta[p] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    float3 n = DecodeNormalOct(g_Normal.Load(int3(p, 0)));
    float2 uv = PixelToUV(p);
    float3 posRel = CameraRayCurrent(uv) * z;

    float4 histS = 0.0f;
    float4 histU = 0.0f;
    float4 histMeta = 0.0f;
    float weightSum = 0.0f;

    float2 prevUV;
    float prevZExpected;
    bool reset = g_Frame.temporalParams.w > 0.5f;
    if (!reset && ProjectPrevious(posRel, prevUV, prevZExpected) && all(prevUV > 0.0f) && all(prevUV < 1.0f))
    {
        float2 prevPixel = prevUV * g_Frame.traceSize.xy - 0.5f;
        int2 base = int2(floor(prevPixel));
        float2 f = prevPixel - float2(base);
        float bilinear[4] = { (1.0f - f.x) * (1.0f - f.y), f.x * (1.0f - f.y), (1.0f - f.x) * f.y, f.x * f.y };
        int2 offsets[4] = { int2(0, 0), int2(1, 0), int2(0, 1), int2(1, 1) };
        float depthReject = g_Frame.temporalParams.y;
        float normalReject = g_Frame.temporalParams.z;

        [unroll]
        for (uint k = 0; k < 4; ++k)
        {
            int2 q = base + offsets[k];
            if (any(q < 0) || any(q >= int2(g_Frame.traceSize.xy)))
                continue;
            float zPrev = g_PrevDepth.Load(int3(q, 0));
            if (zPrev <= 0.0f || abs(zPrev - prevZExpected) > depthReject * prevZExpected)
                continue;
            float3 nPrev = DecodeNormalOct(g_PrevNormal.Load(int3(q, 0)));
            if (dot(nPrev, n) < normalReject)
                continue;
            float w = bilinear[k];
            histS += g_PrevHistS.Load(int3(q, 0)) * w;
            histU += g_PrevHistU.Load(int3(q, 0)) * w;
            histMeta += g_PrevHistMeta.Load(int3(q, 0)) * w;
            weightSum += w;
        }
    }

    float maxHistory = max(g_Frame.temporalParams.x, 1.0f);
    float lumS = Luminance(curS.rgb);
    float historyLength = 0.0f;
    if (weightSum > 1e-3f)
    {
        histS /= weightSum;
        histU /= weightSum;
        histMeta /= weightSum;
        historyLength = histMeta.x;
    }

    historyLength = min(historyLength + 1.0f, maxHistory);
    float alpha = max(1.0f / historyLength, 1.0f / maxHistory);

    float4 outS;
    float3 outU;
    float m1, m2;
    if (historyLength <= 1.0f)
    {
        outS = curS;
        outU = curU.rgb;
        m1 = lumS;
        m2 = lumS * lumS;
    }
    else
    {
        outS = lerp(histS, curS, alpha);
        outU = lerp(histU.rgb, curU.rgb, alpha);
        m1 = lerp(histMeta.y, lumS, alpha);
        m2 = lerp(histMeta.z, lumS * lumS, alpha);
    }

    float variance = max(0.0f, m2 - m1 * m1);
    // Short history: the moment estimate is unreliable, so let the spatial filter blur more.
    if (historyLength < 4.0f)
        variance = max(variance, 4.0f * m1 * m1 / historyLength);

    g_HistS[p] = outS;
    g_HistU[p] = float4(outU, variance);
    g_HistMeta[p] = float4(historyLength, m1, m2, 0.0f);
}
