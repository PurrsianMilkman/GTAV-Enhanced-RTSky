// RTSky - debug blit: the current debug view, drawn into the game's FINAL image (after tone mapping,
// lens effects and upscaling, before the UI), so it overlays cleanly instead of going through the
// game's lighting and post-processing.
//   SRV t0 DebugView (RTSky texture, trace resolution)
//   UAV u0 final image (in place, or the blit copy): store only, every pixel of the viewport is written
//   PassConstants args0 = target viewport (x, y, w, h), args1 = source size (w, h)
//   Dispatch ceil(w / 8) x ceil(h / 8)
#include "Common.hlsli"

Texture2D<float4> g_DebugView : register(t0);
RWTexture2D<float4> g_Target : register(u0);

[numthreads(RTSKY_GROUP_SIZE, RTSKY_GROUP_SIZE, 1)]
void DebugBlitCS(uint3 id : SV_DispatchThreadID)
{
    const uint2 size = g_Pass.args0.zw;
    if (any(id.xy >= size))
        return;
    // Nearest texel: debug views show per-pixel data and must not be smoothed by the upscale.
    const uint2 srcSize = g_Pass.args1.xy;
    const uint2 src = min(uint2((float2(id.xy) + 0.5f) * float2(srcSize) / float2(size)), srcSize - 1);
    const float3 value = saturate(g_DebugView.Load(int3(src, 0)).rgb);
    g_Target[id.xy + g_Pass.args0.xy] = float4(value, 1.0f);
}
