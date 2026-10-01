// RTSky - renderer: GPU resources, pipelines and the two injections
//
//   Prepare   - recorded into the game's command list when the G-buffer pass closes:
//               game depth -> linear depth + normals (RTSky's own textures).
//   Composite - recorded when the HDR lighting pass closes: atmosphere, ray-traced sky visibility
//               against the game's TLAS, temporal + spatial denoising, relighting of the game's
//               HDR target in place.
// Both run on the game's recording thread, inside the game's command list, under HookBypass, and
// restore every piece of state they touch before returning.
#pragma once

#include "../Track/CommandListTracker.h"

#include <string>
#include <vector>

namespace rtsky {
struct Config;
}

namespace rtsky::render {

// Called by the hooks when a render-target binding closes (see track::CloseBinding).
void OnBindingClosed(ID3D12GraphicsCommandList* list, track::ListState& state, const track::BindingRecord& record);

// Called by the ExecuteCommandLists hook before the lists are forwarded (GPU order is known):
// late TLAS binding and the Prepare / Composite pairing check.
void OnSubmit(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists);

// Called once per ScriptHookV tick (game thread): periodic logging.
void OnScriptTick();

// One-line status for the log / hotkey report.
std::string RendererStatus();

// Requests a history reset (camera cut, config reload).
void ResetHistory();

// "off", "1 sky visibility", ... for the debug views (RTSKY_VIEW_*).
const char* DebugViewName(int view);

// Forgets the camera / TLAS-space calibration (hotkey).
void ResetCalibration();

// Status lines for the on-screen overlay: a one-line verdict (what RTSky is doing, or the first
// thing stopping it), then details.
std::vector<std::string> OverlayLines(const Config& cfg);

} // namespace rtsky::render
