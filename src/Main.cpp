// RTSky - entry point
//
// Loaded as an .asi by an ASI loader (ScriptHookV's xinput1_4.dll for GTA V Enhanced, or Ultimate
// ASI Loader). DllMain only does loader-lock-safe work: logging, config, the D3D12CreateDevice
// detour and the ScriptHookV script registration. Everything else happens in the script fiber
// (natives) and in the D3D12 hooks (rendering).
#include <windows.h>

#include "Common/Config.h"
#include "Common/Log.h"
#include "Game/GameData.h"
#include "Game/Overlay.h"
#include "Game/ScriptHookV.h"
#include "Hooks/D3D12Hooks.h"
#include "Render/Renderer.h"
#include "Track/FrameAnalyzer.h"
#include "Track/ShaderCapture.h"
#include "Track/TlasTracker.h"

#include "../shaders/RTSkyShared.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

HMODULE g_module = nullptr;
std::wstring g_directory;

// Key presses from ScriptHookV's keyboard handler (window thread) -> script fiber
std::atomic<int> g_pressedKey{ 0 };

std::wstring ModuleDirectory(HMODULE module)
{
    wchar_t path[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(module, path, MAX_PATH);
    std::wstring s(path, n);
    size_t slash = s.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : s.substr(0, slash + 1);
}

void ApplyConfig()
{
    const rtsky::Config cfg = rtsky::ConfigSnapshot();
    rtsky::track::Analyzer().Configure(cfg.compositeCandidate, cfg.gbufferOrdinal, cfg.compositeOrdinal, cfg.stableFrames);
    rtsky::track::Tlas().SetCloneEnabled(cfg.tlasClone);
    rtsky::track::Tlas().SetSelect(cfg.tlasSelect);
    rtsky::track::SetShaderCaptureDirectory(cfg.captureShaders ? g_directory + L"RTSky_shaders" : std::wstring());
}

void OnKeyboard(DWORD key, WORD, BYTE, BOOL, BOOL, BOOL wasDownBefore, BOOL isUpNow)
{
    if (isUpNow || wasDownBefore)
        return;
    g_pressedKey.store(static_cast<int>(key));
}

std::string KeyName(int vk)
{
    if (vk >= 0x60 && vk <= 0x69)
        return "Num" + std::to_string(vk - 0x60);
    switch (vk)
    {
    case 0x6A: return "Num*";
    case 0x6B: return "Num+";
    case 0x6D: return "Num-";
    case 0x6E: return "Num.";
    case 0x6F: return "Num/";
    default: break;
    }
    if (vk >= 0x70 && vk <= 0x7B)
        return "F" + std::to_string(vk - 0x6F);
    if (vk <= 0)
        return "-";
    char buf[16];
    snprintf(buf, sizeof(buf), "0x%02X", vk);
    return buf;
}

const char* OnOff(bool v)
{
    return v ? "ON" : "off";
}

void Notify(const std::string& text)
{
    rtsky::game::overlay::Toast("RTSky: " + text);
    LOG_INFO("Hotkey: %s", text.c_str());
}

void HandleHotkeys()
{
    using rtsky::Config;
    const int key = g_pressedKey.exchange(0);
    if (key <= 0)
        return;
    const Config cfg = rtsky::ConfigSnapshot();

    if (key == cfg.keyToggle)
    {
        const Config c = rtsky::UpdateConfig([](Config& x) { x.enabled = !x.enabled; });
        rtsky::render::ResetHistory();
        Notify(c.enabled ? "ON" : "OFF");
    }
    else if (key == cfg.keyOverlay)
    {
        const Config c = rtsky::UpdateConfig([](Config& x) { x.overlay = !x.overlay; });
        Notify(std::string("status overlay ") + OnOff(c.overlay));
    }
    else if (key == cfg.keyCompare)
    {
        const Config c = rtsky::UpdateConfig([](Config& x) { x.compareSplit = !x.compareSplit; });
        Notify(c.compareSplit ? "split compare ON (left: original, right: RTSky)" : "split compare off");
    }
    else if (key == cfg.keyForceRelight)
    {
        const Config c = rtsky::UpdateConfig([](Config& x) { x.forceRelight = !x.forceRelight; });
        Notify(c.forceRelight ? "force relight ON (ignores the calibration)" : "force relight off");
    }
    else if (key == cfg.keyDebugView)
    {
        const Config c = rtsky::UpdateConfig([](Config& x) { x.debugView = (x.debugView + 1) % RTSKY_VIEW_COUNT; });
        Notify(std::string("debug view ") + rtsky::render::DebugViewName(c.debugView));
    }
    else if (key == cfg.keySunShadows)
    {
        const Config c = rtsky::UpdateConfig([](Config& x) { x.sunShadowRays = !x.sunShadowRays; });
        rtsky::render::ResetHistory();
        Notify(std::string("sun / moon shadow rays ") + OnOff(c.sunShadowRays));
    }
    else if (key == cfg.keyFoliage)
    {
        const Config c = rtsky::UpdateConfig([](Config& x) {
            x.foliageMode = x.foliageMode == rtsky::FoliageMode::Stochastic ? rtsky::FoliageMode::Opaque
                          : x.foliageMode == rtsky::FoliageMode::Opaque     ? rtsky::FoliageMode::Ignore
                                                                            : rtsky::FoliageMode::Stochastic;
        });
        rtsky::render::ResetHistory();
        Notify(std::string("foliage ") + (c.foliageMode == rtsky::FoliageMode::Opaque   ? "opaque"
                                          : c.foliageMode == rtsky::FoliageMode::Ignore ? "ignored"
                                                                                        : "stochastic"));
    }
    else if (key == cfg.keyNearField)
    {
        const Config c = rtsky::UpdateConfig([](Config& x) { x.nearField = !x.nearField; });
        rtsky::render::ResetHistory();
        Notify(std::string("near-field split ") + OnOff(c.nearField));
    }
    else if (key == cfg.keyTracePath)
    {
        const Config c = rtsky::UpdateConfig([](Config& x) {
            x.tracePath = x.tracePath == rtsky::TracePath::Pipeline ? rtsky::TracePath::Inline : rtsky::TracePath::Pipeline;
        });
        rtsky::render::ResetHistory();
        Notify(c.tracePath == rtsky::TracePath::Pipeline ? "trace path: DXR pipeline" : "trace path: inline RayQuery");
    }
    else if (key == cfg.keyReload)
    {
        rtsky::ReloadConfig();
        ApplyConfig();
        rtsky::render::ResetHistory();
        Notify("RTSky.ini reloaded");
    }
    else if (key == cfg.keyDumpFrame)
    {
        rtsky::track::Analyzer().RequestDump(g_directory + L"RTSky_frame.log");
        Notify("frame dump requested (RTSky_frame.log)");
    }
    else if (key == cfg.keyStrengthUp || key == cfg.keyStrengthDown)
    {
        const float step = key == cfg.keyStrengthUp ? 0.1f : -0.1f;
        const Config c = rtsky::UpdateConfig([step](Config& x) {
            x.strength = std::round(std::clamp(x.strength + step, 0.0f, 1.0f) * 100.0f) / 100.0f;
        });
        char buf[64];
        snprintf(buf, sizeof(buf), "strength %.2f", c.strength);
        Notify(buf);
    }
    else if (key == cfg.keyDenoiser)
    {
        const Config c = rtsky::UpdateConfig([](Config& x) { x.denoiser = !x.denoiser; });
        Notify(std::string("spatial denoiser ") + OnOff(c.denoiser));
    }
    else if (key == cfg.keyReset)
    {
        rtsky::render::ResetHistory();
        rtsky::render::ResetCalibration();
        Notify("history and calibration reset");
    }
}

// Status overlay: the renderer's lines plus the key legend.
void DrawOverlay(const rtsky::Config& cfg)
{
    std::vector<std::string> lines;
    if (cfg.overlay)
    {
        lines = rtsky::render::OverlayLines(cfg);
        lines.push_back("Keys: " + KeyName(cfg.keyToggle) + " on/off  " + KeyName(cfg.keyOverlay) + " overlay  " +
                        KeyName(cfg.keyCompare) + " compare  " + KeyName(cfg.keyForceRelight) + " force  " + KeyName(cfg.keyDebugView) +
                        " view  " + KeyName(cfg.keySunShadows) + " sun  " + KeyName(cfg.keyFoliage) + " foliage");
        lines.push_back("      " + KeyName(cfg.keyNearField) + " near-field  " + KeyName(cfg.keyTracePath) + " path  " +
                        KeyName(cfg.keyReload) + " reload  " + KeyName(cfg.keyDumpFrame) + " dump  " + KeyName(cfg.keyStrengthUp) +
                        "/" + KeyName(cfg.keyStrengthDown) + " strength  " + KeyName(cfg.keyDenoiser) + " denoise  " +
                        KeyName(cfg.keyReset) + " reset   (NumLock on)");
    }
    rtsky::game::overlay::Draw(lines);
}

void ScriptMain()
{
    LOG_INFO("Script thread started");
    const ULONGLONG start = GetTickCount64();
    ULONGLONG lastVerify = start;
    for (;;)
    {
        rtsky::game::Game().Tick();

        const ULONGLONG now = GetTickCount64();
        // If the game created its device before we were loaded, the early detour never fired.
        if (!rtsky::hooks::Installed() && now - start > 5000)
            rtsky::hooks::InstallLate();
        if (now - lastVerify > 2000)
        {
            lastVerify = now;
            rtsky::hooks::VerifyHooks();
        }

        HandleHotkeys();
        DrawOverlay(rtsky::ConfigSnapshot());
        rtsky::render::OnScriptTick();
        rtsky::game::SHV().ScriptWait(0);
    }
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(instance);
        g_module = instance;
        // Never unloaded: vtable hooks, the loader notification, the MinHook detour and deleters of
        // in-flight GPU work all point into this module, so a FreeLibrary must not unmap it.
        HMODULE pinned = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&DllMain), &pinned);
        g_directory = ModuleDirectory(instance);

        rtsky::log::Init((g_directory + L"RTSky.log").c_str());
        rtsky::SetConfigPath(g_directory + L"RTSky.ini");
        rtsky::ReloadConfig();
        ApplyConfig();
        rtsky::track::Analyzer().SetAutoDumpPath(g_directory + L"RTSky_frame.log");
        rtsky::track::Analyzer().SetPipelineNamer(&rtsky::track::PixelShaderHash);

        LOG_INFO("RTSky %s - ray-traced sky lighting for GTA V Enhanced", RTSKY_VERSION);
        LOG_INFO("At load: d3d12.dll %s, dxgi.dll %s, sl.interposer.dll %s",
                 GetModuleHandleW(L"d3d12.dll") ? "loaded" : "not loaded", GetModuleHandleW(L"dxgi.dll") ? "loaded" : "not loaded",
                 GetModuleHandleW(L"sl.interposer.dll") ? "loaded" : "not loaded");

        rtsky::hooks::InstallEarly();

        if (rtsky::game::SHV().Load())
        {
            rtsky::game::SHV().ScriptRegister(instance, &ScriptMain);
            rtsky::game::SHV().KeyboardHandlerRegister(&OnKeyboard);
        }
        else
        {
            LOG_ERROR("ScriptHookV is required (camera, clock and weather come from game natives). RTSky stays inactive.");
        }
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        if (rtsky::game::SHV().IsLoaded())
        {
            rtsky::game::SHV().KeyboardHandlerUnregister(&OnKeyboard);
            rtsky::game::SHV().ScriptUnregister(instance);
        }
        // The module is pinned, so this only runs at process exit: other threads are gone and the
        // hooks stay in place (restoring them would race nothing but gains nothing either).
        LOG_INFO("RTSky unloaded");
        rtsky::log::Shutdown();
    }
    return TRUE;
}
