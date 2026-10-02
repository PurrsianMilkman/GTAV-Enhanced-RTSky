// RTSky - input census of the game's deferred lighting pass (PS_directional_standard)
//
// At the first draw of the lighting pass, RTSky names every input its sky-term replacement will need:
// the resources behind the G-buffer, AO and RTGI registers (descriptor resolution through the captured
// root signature), the game's constant buffers (root CBV or descriptor, the buffer and heap behind the
// address), whether each input's state is known in that list, and the runtime values of the sky
// ambient constants (read on the CPU from the upload buffer; nothing is bound on the GPU). Logged as
// [Names] / [RootSig] / [Inputs] / [States] once, and [GameCB] every 10 s. Diagnostics only.
#pragma once

namespace rtsky::track {
struct ListState;
}

namespace rtsky::render {

// From the draw hooks, after track::OnDraw: cheap unless a census is due.
void OnLightingDraw(const track::ListState& state);

} // namespace rtsky::render
