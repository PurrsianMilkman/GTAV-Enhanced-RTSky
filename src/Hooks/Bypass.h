// RTSky - hook bypass
// RTSky's own D3D12 calls go through the same patched vtables as the game's. Every hook body first
// checks HookBypass::Active() and, if set, forwards straight to the original without tracking, so
// nothing RTSky records is mistaken for game state.
#pragma once

namespace rtsky::hooks {

class HookBypass
{
public:
    HookBypass() { ++t_depth; }
    ~HookBypass() { --t_depth; }
    HookBypass(const HookBypass&) = delete;
    HookBypass& operator=(const HookBypass&) = delete;

    static bool Active() { return t_depth > 0; }

private:
    static inline thread_local int t_depth = 0;
};

} // namespace rtsky::hooks
