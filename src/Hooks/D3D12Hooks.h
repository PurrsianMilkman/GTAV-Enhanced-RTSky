// RTSky - D3D12 hook installation
#pragma once

#include <d3d12.h>

namespace rtsky::hooks {

// Called from DllMain (loader lock held: only MinHook + loader notifications). Detours
// d3d12.dll!D3D12CreateDevice as soon as d3d12.dll is (or gets) loaded, so that RTSky sees the
// game's device - and every render target the game creates - from the very start.
bool InstallEarly();

// Fallback when the game created its device before RTSky was loaded: finds the device (D3D12
// devices are singletons per adapter) and patches the vtables. Render targets created before this
// point are unknown until the game recreates them (resolution or window-mode change).
bool InstallLate();

// True once the device / command list / queue vtables are patched.
bool Installed();

// Re-applies vtable entries that were overwritten (D3D12 runtime-bypass vtable swaps). Cheap.
void VerifyHooks();

void Uninstall();

} // namespace rtsky::hooks
