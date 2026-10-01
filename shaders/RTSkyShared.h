// RTSky - ray-traced sky lighting for GTA V Enhanced
// Shared between C++ (host) and HLSL (shaders). Every member is a 16-byte vector so that the C++
// layout is identical to HLSL constant-buffer / structured-buffer packing.
#ifndef RTSKY_SHARED_H
#define RTSKY_SHARED_H

#ifdef __cplusplus
#include <cstdint>
namespace rtsky::gpu {
struct float4 { float x, y, z, w; };
struct uint4 { uint32_t x, y, z, w; };
#define RTSKY_STATIC_ASSERT(c, m) static_assert(c, m)
#else
#define RTSKY_STATIC_ASSERT(c, m)
#endif

// ---------------------------------------------------------------------------------------------
// Constants shared by shaders and host
// ---------------------------------------------------------------------------------------------
#define RTSKY_TRANSMITTANCE_W 256
#define RTSKY_TRANSMITTANCE_H 64
#define RTSKY_MULTISCATTER_SIZE 32
#define RTSKY_SKYVIEW_W 192
#define RTSKY_SKYVIEW_H 108
#define RTSKY_SH_DIRECTIONS 1024      // Fibonacci directions on the upper hemisphere (SkyProjectCS)
#define RTSKY_SKYPROJECT_THREADS 64 
#define RTSKY_PROBE_GRID 8            // 8x8 probe pixels
#define RTSKY_PROBE_HYPOTHESES 8      // camera latency 0..3 x TLAS space {world, camera-relative}
#define RTSKY_GROUP_SIZE 8            // 8x8 thread groups for all screen-space passes

// FrameConstants.traceFlags.w bits
#define RTSKY_FLAG_SUN_RAYS        0x1u   // trace one sun/moon visibility ray per pixel
#define RTSKY_FLAG_FOLIAGE_OPAQUE  0x2u   // RAY_FLAG_FORCE_OPAQUE
#define RTSKY_FLAG_FOLIAGE_IGNORE  0x4u   // RAY_FLAG_CULL_NON_OPAQUE
                                          // neither: stochastic foliage coverage (any-hit)
#define RTSKY_FLAG_DEBUG_TLAS      0x8u   // trace pass writes primary-ray agreement into TraceS
#define RTSKY_FLAG_REVERSED_Z      0x10u  // informational, depth mode is camForward.w

// FrameConstants.camForward.w: depth encoding
#define RTSKY_DEPTH_REVERSED_FINITE   0
#define RTSKY_DEPTH_REVERSED_INFINITE 1
#define RTSKY_DEPTH_STANDARD          2

// Debug views (FrameConstants.compositeParams3.z)
#define RTSKY_VIEW_NONE        0
#define RTSKY_VIEW_SKY_RATIO   1
#define RTSKY_VIEW_SUN_VIS     2
#define RTSKY_VIEW_NORMALS     3
#define RTSKY_VIEW_DEPTH       4
#define RTSKY_VIEW_SKY_S       5
#define RTSKY_VIEW_TLAS        6
#define RTSKY_VIEW_RATIO       7
#define RTSKY_VIEW_HISTORY     8
#define RTSKY_VIEW_COUNT       9

// ---------------------------------------------------------------------------------------------
// FrameConstants (root CBV b0). Positions are camera-relative (world axes, metres) unless noted.
// ---------------------------------------------------------------------------------------------
struct FrameConstants
{
    // --- current camera (world axes; GTA V: Z up) ---
    float4 camPos;          // xyz camera world position (only used for sky altitude / hashing), w 0
    float4 camRight;        // xyz = right   * tan(fovX/2), w = near plane (m)
    float4 camUp;           // xyz = up      * tan(fovY/2), w = far plane (m)
    float4 camForward;      // xyz = forward (unit),        w = depth mode (RTSKY_DEPTH_*)

    // --- previous frame camera (for reprojection) ---
    float4 prevCamOffset;   // xyz = prevCamPos - camPos, w 0
    float4 prevCamRight;    // as camRight (w = near)
    float4 prevCamUp;       // as camUp    (w = far)
    float4 prevCamForward;  // as camForward

    // --- TLAS space: tlasPos = cameraRelativePos + tlasOffset ---
    float4 tlasOffset;      // xyz = camPos - tlasOrigin, w 0

    // --- resolution / viewport mapping ---
    float4 traceSize;       // x = W, y = H, z = 1/W, w = 1/H (trace resolution == depth viewport size)
    float4 depthViewport;   // xy = viewport origin in the game depth texture (px), zw = depth texture size (px)
    float4 targetViewport;  // xy = viewport origin in the HDR target (px), zw = viewport size (px)

    // --- lights (directions point TOWARDS the light, world axes) ---
    float4 sunDir;          // xyz, w = cone half-angle used for soft shadow rays (rad)
    float4 moonDir;         // xyz, w = cone half-angle (rad)
    float4 lightSelect;     // x = 1: shadow light is the sun, 0: moon; y = sun visibility above horizon [0,1]
                            // z = moon/sun radiance ratio used for the night sky, w 0

    // --- atmosphere (Hillaire 2020), distances in km ---
    float4 atmoRadii;       // x = bottom radius, y = top radius, z = camera altitude above bottom, w = Mie g
    float4 rayleighScattering; // rgb (1/km), w = Rayleigh scale height (km)
    float4 mieParams;       // x = Mie scattering, y = Mie extinction, z = Mie absorption (1/km), w = Mie scale height (km)
    float4 ozoneAbsorption; // rgb (1/km), w = ozone layer centre altitude (km)
    float4 atmoMisc;        // x = ozone layer width (km), y = atmosphere ground albedo, z = multi-scattering factor, w 0
    float4 solarIlluminance;// rgb solar illuminance at the top of the atmosphere (relative units), w 0
    float4 moonIlluminance; // rgb moon illuminance (same units), w 0

    // --- weather ---
    float4 weather;         // x = cloudiness c [0,1], y = overcast transmission, z = direct light factor, w = wetness

    // --- trace ---
    float4 traceParams;     // x = max ray distance (m), y = normal bias (m), z = distance bias (m/m), w = tMin (m)
    float4 foliageParams;   // x = foliage opacity [0,1], y = foliage grid cells per triangle edge,
                            // z = near-field radius (m) excluded from R (handled by game AO), w 0
    uint4  traceFlags;      // x = rays per pixel, y = instance inclusion mask, z = frame index, w = RTSKY_FLAG_*

    // --- temporal / spatial filter ---
    float4 temporalParams;  // x = max history, y = depth reject (relative), z = normal reject (cos), w = 1: reset history
    float4 denoiseParams;   // x = sigma plane, y = normal power, z = sigma luminance, w 0

    // --- composite ---
    float4 compositeParams;  // x = strength, y = min ratio, z = max ratio, w = game sky occlusion assumption
    float4 compositeParams2; // x = direct scale, y = artificial ambient (solar units), z = ground albedo, w = emissive threshold
    float4 compositeParams3; // x = global fade [0,1], y = near fade distance (m), z = debug view, w = HDR target is sRGB-encoded (1) or linear (0)
    float4 compositeParams4; // x = distance fade start (m), y = distance fade end (m), z = light is sun (1) / moon (0) for E_dir, w 0

    // --- calibration probe: 8 hypothesis cameras ---
    // probeCam[h*4+0].xyz = hypothesis camera offset relative to the CURRENT camera position,
    //                     w = 1 if TLAS is camera-relative for this hypothesis
    // probeCam[h*4+1] = right * tanX, [h*4+2] = up * tanY, [h*4+3] = forward
    float4 probeCam[RTSKY_PROBE_HYPOTHESES * 4];
};
RTSKY_STATIC_ASSERT(sizeof(FrameConstants) % 16 == 0, "FrameConstants must be float4 aligned");
RTSKY_STATIC_ASSERT(sizeof(FrameConstants) <= 1024, "FrameConstants ring slots are 1024 bytes");

// ---------------------------------------------------------------------------------------------
// PassConstants (root 32-bit constants b1, 8 dwords)
// ---------------------------------------------------------------------------------------------
struct PassConstants
{
    uint4 args0;   // ATrous: x = iteration, y = step width; generic otherwise
    uint4 args1;
};

// ---------------------------------------------------------------------------------------------
// SkyData (structured buffer written by SkyProjectCS, read by trace + composite)
// ---------------------------------------------------------------------------------------------
struct SkyData
{
    float4 sh[9];              // rgb = cosine-convolved SH L2 coefficients of the upper-hemisphere sky:
                               //       E(n) = sum_i sh[i].rgb * Y_i(n), w unused
    float4 sunIrradiance;      // rgb illuminance of the sun on a surface facing it (after atmosphere + weather)
    float4 moonIrradiance;     // rgb, same for the moon
    float4 skyHorizontal;      // rgb irradiance of the sky on an upward-facing surface, w = clear-sky value luminance
    float4 overcast;           // rgb = overcast zenith radiance L_z, w = cloudiness c used
};

#ifdef __cplusplus
} // namespace rtsky::gpu
#endif

// ---------------------------------------------------------------------------------------------
// Register assignments per pass (descriptor tables: SRV t0..t11, UAV u0..u7; TLAS t0 space1)
// ---------------------------------------------------------------------------------------------
// TRANSMITTANCE  UAV u0 TransmittanceLut
// MULTISCATTER   SRV t0 TransmittanceLut             UAV u0 MultiScatterLut
// SKYVIEW        SRV t0 TransmittanceLut, t1 MultiScatterLut          UAV u0 SkyViewLut
// SKYPROJECT     SRV t0 SkyViewLut, t1 TransmittanceLut               UAV u0 SkyData (RWStructuredBuffer)
// PREPARE        SRV t0 game depth (plane 0, R32 float view)          UAV u0 LinearDepth[cur], u1 Normal[cur]
// TRACE          SRV t0 LinearDepth[cur], t1 Normal[cur], t2 SkyViewLut, t3 SkyData
//                UAV u0 TraceS, u1 TraceU
// TEMPORAL       SRV t0 TraceS, t1 TraceU, t2 LinearDepth[cur], t3 Normal[cur], t4 LinearDepth[prev],
//                    t5 Normal[prev], t6 HistS[prev], t7 HistU[prev], t8 HistMeta[prev]
//                UAV u0 HistS[cur], u1 HistU[cur], u2 HistMeta[cur]
// ATROUS i       SRV t0 inS, t1 inU, t2 LinearDepth[cur], t3 Normal[cur]   UAV u0 outS, u1 outU
// COMPOSITE      SRV t0 FiltS[final], t1 FiltU[final], t2 LinearDepth[cur], t3 Normal[cur], t4 SkyData,
//                    t5 HistMeta[cur], t6 TraceS
//                UAV u0 HDR colour (game target in place, or SceneCopy)
// PROBE          SRV t0 LinearDepth[cur]                                UAV u0 ProbeResults (uint[64])
//
// Resource contents:
//   TraceS   rgb = S (sky radiance * visibility, averaged over rays), a = sun/moon visibility
//   TraceU   rgb = U (sky radiance, same rays, near-field visible),  a = 1 valid pixel / 0 invalid
//   HistS    rgb = accumulated S, a = accumulated visibility
//   HistU    rgb = accumulated U, a = luminance variance of S
//   HistMeta x = history length, y = E[lum S], z = E[lum S^2], w = 0
//   FiltS/U  same layout as HistS/HistU (FiltU.a = filtered variance)
//   ProbeResults[h] = number of probe rays of hypothesis h whose TLAS hit matches depth,
//   ProbeResults[8 + h] = number of valid probe rays evaluated for hypothesis h,
//   ProbeResults[16] = number of probe pixels with valid depth

#endif // RTSKY_SHARED_H
