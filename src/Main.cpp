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
#include "Game/ScriptHookV.h"
#include "Hooks/D3D12Hooks.h"
#include "Render/Renderer.h"
#include "Track/FrameAnalyzer.h"
#include "Track/TlasTracker.h"

#include "../shaders/RTSkyShared.h"

#include <atomic>
#include <string>

namespace {

HMODULE g_module = nullptr;
std::wstring g_directory;

// Key presses from ScriptHookV's keyboard handler (window thread) -> script fiber
std::atomic<int> g_pressedKey{ 0 };
std::atomic<bool> g_pressedWithCtrl{ false };

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
}

void OnKeyboard(DWORD key, WORD, BYTE, BOOL, BOOL, BOOL wasDownBefore, BOOL isUpNow)
{
    if (isUpNow || wasDownBefore)
        return;
    g_pressedWithCtrl.store((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0);
    g_pressedKey.store(static_cast<int>(key));
}

void HandleHotkeys()
{
    const int key = g_pressedKey.exchange(0);
    if (key == 0)
        return;
    const bool ctrl = g_pressedWithCtrl.load();
    const rtsky::Config cfg = rtsky::ConfigSnapshot();

    if (ctrl && key == cfg.keyReload)
    {
        rtsky::ReloadConfig();
        ApplyConfig();
        rtsky::render::ResetHistory();
        LOG_INFO("Configuration reloaded");
    }
    else if (ctrl && key == cfg.keyDumpFrame)
    {
        rtsky::track::Analyzer().RequestDump(g_directory + L"RTSky_frame.log");
        LOG_INFO("Frame dump requested");
    }
    else if (!ctrl && key == cfg.keyToggle)
    {
        rtsky::SetConfigEnabled(!cfg.enabled);
        rtsky::render::ResetHistory();
        LOG_INFO("RTSky %s", cfg.enabled ? "disabled" : "enabled");
    }
    else if (!ctrl && key == cfg.keyDebugView)
    {
        const int view = (cfg.debugView + 1) % RTSKY_VIEW_COUNT;
        rtsky::SetConfigDebugView(view);
        LOG_INFO("Debug view %d", view);
    }
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
