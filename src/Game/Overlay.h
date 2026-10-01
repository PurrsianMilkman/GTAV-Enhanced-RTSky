// RTSky - on-screen text drawn with the game's own UI natives (script thread only)
//
// A few status lines in the top-left corner and a short confirmation ("toast") after each hotkey.
// The text is part of the game's HUD, so it is never relit and never shows up in RTSky's passes.
#pragma once

#include <string>
#include <vector>

namespace rtsky::game::overlay {

// Shows `text` for a couple of seconds (call from any thread).
void Toast(const std::string& text);

// Draws the status lines (if any) and the current toast. Call once per script tick.
void Draw(const std::vector<std::string>& lines);

} // namespace rtsky::game::overlay
