// RTSky - configuration (RTSky.ini next to the .asi)
#pragma once

#include <cstdint>
#include <string>

namespace rtsky {

enum class TracePath : int { Pipeline = 0, Inline = 1 };
enum class FoliageMode : int { Stochastic = 0, Opaque = 1, Ignore = 2 };
enum class DepthModeSetting : int { Auto = -1, ReversedFinite = 0, ReversedInfinite = 1, Standard = 2 };
enum class TlasSpaceSetting : int { Auto = -1, World = 0, CameraRelative = 1 };

struct Config
{
    // [General]
    bool enabled = true;
    int logLevel = 2; // 0 error, 1 warning, 2 info, 3 debug

    // [Hotkeys] (virtual-key codes; Ctrl modifier for reload / dump)
    int keyToggle = 0x79;      // F10
    int keyDebugView = 0x7A;   // F11
    int keyReload = 0x79;      // Ctrl+F10
    int keyDumpFrame = 0x7A;   // Ctrl+F11

    // [Trace]
    TracePath tracePath = TracePath::Pipeline;
    int raysPerPixel = 1;
    float maxRayDistance = 400.0f;
    float normalBias = 0.03f;
    float distanceBias = 0.002f;
    float tMin = 0.01f;
    float nearFieldRadius = 1.0f;
    uint32_t instanceMask = 0xFF;
    FoliageMode foliageMode = FoliageMode::Stochastic;
    float foliageOpacity = 0.6f;
    float foliageCells = 6.0f;
    bool sunShadowRays = true;
    float sunSoftness = 4.0f;

    // [Denoise]
    float maxHistory = 32.0f;
    float depthReject = 0.05f;
    float normalReject = 0.9f;
    int denoiseIterations = 4;
    float sigmaPlane = 0.02f;
    float normalPower = 64.0f;
    float sigmaLuminance = 4.0f;

    // [Composite]
    float strength = 0.85f;
    float minRatio = 0.3f;
    float maxRatio = 1.0f;
    float gameSkyOcclusion = 1.0f;
    float directScale = 1.0f;
    float artificialAmbient = 0.02f;
    float groundAlbedo = 0.2f;
    float nearFadeDistance = 0.5f;
    float fadeStart = 250.0f;
    float fadeEnd = 500.0f;
    float interiorStrength = 0.0f;
    float cutsceneStrength = 1.0f;
    int debugView = 0;

    // [Camera]
    int cameraLatency = -1; // -1 = auto (calibration probe), else 0..3
    TlasSpaceSetting tlasSpace = TlasSpaceSetting::Auto;
    DepthModeSetting depthMode = DepthModeSetting::Auto;
    float fovScale = 1.0f;
    bool calibrationProbe = true;
    float minCalibrationScore = 0.35f; // relighting starts once the probe agrees this well

    // [Detection]
    int gbufferOrdinal = -1;       // -1 auto, else ordinal of the MRT binding in its command list
    int compositeCandidate = 0;    // n-th float HDR binding after the G-buffer (execution order)
    int compositeOrdinal = -1;     // -1 auto, else per-list ordinal of the HDR binding to hook
    int tlasSelect = -1;           // -1 auto (largest recent), else n-th distinct TLAS
    bool tlasClone = true;
    int stableFrames = 3;

    // [Sky]
    float sunIntensity = 1.0f;
    float moonIntensity = 2.5e-6f;
    float mieScale = 1.0f;
    float atmosphereGroundAlbedo = 0.3f;
    float sunRollDeg = 122.0f;      // time.xml <suninfo sun_roll>
    float dayStartHour = 6.0f;      // sunrise
    float dayLengthHours = 14.0f;   // sunrise -> sunset
    float sunAzimuthOffsetDeg = 0.0f;
    float moonRollDeg = 122.0f;

    bool Load(const std::wstring& path);
};

// Global configuration. Written by the script thread (load / reload), read by render threads.
// Reads take a copy under the lock (Snapshot) so a reload never tears a frame's parameters.
Config ConfigSnapshot();
bool ReloadConfig();
void SetConfigPath(const std::wstring& path);
void SetConfigDebugView(int view);
void SetConfigEnabled(bool enabled);

} // namespace rtsky
