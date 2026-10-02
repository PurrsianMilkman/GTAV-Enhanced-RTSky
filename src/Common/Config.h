// RTSky - configuration (RTSky.ini next to the .asi)
#pragma once

#include <cstdint>
#include <functional>
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

    // [Hotkeys] virtual-key codes (the INI also accepts names such as Numpad1, NumpadAdd, F10).
    // Numpad digits need NumLock on.
    int keyToggle = 0x61;       // Numpad1: RTSky on / off
    int keyOverlay = 0x62;      // Numpad2: on-screen status
    int keyCompare = 0x63;      // Numpad3: split-screen compare (left original, right RTSky)
    int keyForceRelight = 0x64; // Numpad4: relight even while calibration has not passed
    int keyDebugView = 0x65;    // Numpad5: next debug view
    int keySunShadows = 0x66;   // Numpad6: sun / moon shadow rays
    int keyFoliage = 0x67;      // Numpad7: foliage mode
    int keyNearField = 0x68;    // Numpad8: near-field split
    int keyTracePath = 0x69;    // Numpad9: DXR pipeline / inline ray query
    int keyReload = 0x60;       // Numpad0: reload RTSky.ini
    int keyDumpFrame = 0x6E;    // Numpad . : frame dump
    int keyStrengthUp = 0x6B;   // Numpad + : strength +0.1
    int keyStrengthDown = 0x6D; // Numpad - : strength -0.1
    int keyDenoiser = 0x6A;     // Numpad * : spatial denoiser
    int keyReset = 0x6F;        // Numpad / : reset history and calibration

    // [Display]
    bool overlay = true;        // on-screen status lines (top left)
    bool compareSplit = false;  // left half of the screen untouched, right half relit

    // Runtime toggles (hotkeys only; a reload restores the INI values)
    bool nearField = true;      // false: NearFieldRadius treated as 0
    bool denoiser = true;       // false: no a-trous iterations

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
    bool forceRelight = false;         // relight even while the calibration has not passed

    // [Detection]
    int gbufferOrdinal = -1;       // -1 auto, else ordinal of the MRT binding in its command list
    int compositeCandidate = 0;    // n-th float HDR binding after the G-buffer (execution order)
    int compositeOrdinal = -1;     // -1 auto, else per-list ordinal of the HDR binding to hook
    // Pixel-shader entry name of the pass the Composite goes after ("" or "ordinal" = the format /
    // ordinal rule only; CompositeOrdinal pins that rule too).
    std::string compositePass = "PS_directional_standard";
    int tlasSelect = -1;           // -1 auto (largest recent), else n-th distinct TLAS
    bool tlasClone = true;
    int stableFrames = 3;
    bool captureShaders = false;   // write the game's pixel shaders to RTSky_shaders (diagnostics)

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
// Applies a change to the live configuration (hotkeys); returns the updated copy.
Config UpdateConfig(const std::function<void(Config&)>& edit);

} // namespace rtsky
