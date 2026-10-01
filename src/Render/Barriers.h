// RTSky - barriers
//   * RTSky's own resources: legacy state transitions batched per pass (OwnStates).
//   * Game-owned resources: transitions expressed as abstract "usages" and issued with the same
//     barrier API the game uses on that resource (legacy ResourceBarrier or enhanced Barrier), so a
//     subresource is never switched between the two models (which would require a detour via COMMON).
#pragma once

#include <d3d12.h>

#include <cstdint>
#include <vector>

namespace rtsky::render {

enum class Usage : uint8_t
{
    RenderTarget,
    DepthWrite,
    DepthRead,
    ComputeRead,      // SRV in a compute / ray tracing shader
    ComputeWrite,     // UAV in a compute shader
    CopySource,
    CopyDest,
    Common,
};

struct GameResourceState
{
    Usage usage = Usage::RenderTarget;
    bool enhanced = false;
    // Legacy: the exact D3D12_RESOURCE_STATES the resource is in. Enhanced: the exact layout.
    D3D12_RESOURCE_STATES legacyState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    D3D12_BARRIER_LAYOUT layout = D3D12_BARRIER_LAYOUT_RENDER_TARGET;
    // Enhanced only: the SyncAfter / AccessAfter of the game's last barrier on the resource in this
    // list, when one was observed. RTSky's transition waits for that scope and its restore re-opens
    // exactly that scope for the game's following work. Without an observation, every access the
    // layout allows is assumed and SYNC_ALL is used.
    bool scopeObserved = false;
    D3D12_BARRIER_SYNC sync = D3D12_BARRIER_SYNC_ALL;
    D3D12_BARRIER_ACCESS access = D3D12_BARRIER_ACCESS_COMMON;
};

D3D12_RESOURCE_STATES LegacyStateFor(Usage u);
D3D12_BARRIER_LAYOUT LayoutFor(Usage u);
D3D12_BARRIER_SYNC SyncFor(Usage u);
D3D12_BARRIER_ACCESS AccessFor(Usage u);
// Converts an observed layout back to a usage (best effort, for layouts RTSky knows how to leave).
bool UsageFromLayout(D3D12_BARRIER_LAYOUT layout, Usage* out);
bool UsageFromLegacyState(D3D12_RESOURCE_STATES state, Usage* out);

// Transition of a game resource between two states. depthPlaneOnly restricts the barrier to
// subresource 0 (legacy) / mip 0, slice 0, plane 0 (enhanced); otherwise it covers the whole resource,
// which RTSky only does for single-subresource targets. A UAV -> UAV "transition" emits a UAV barrier.
void TransitionGame(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, const GameResourceState& from,
                    Usage to, bool depthPlaneOnly);
// Returns the resource from `to` back to exactly `original`.
void RestoreGame(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, Usage current, const GameResourceState& original,
                 bool depthPlaneOnly);

// Batches legacy transitions for RTSky's own resources.
class OwnStates
{
public:
    void Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);
    void Uav(ID3D12Resource* resource);
    void Flush(ID3D12GraphicsCommandList* list);

private:
    std::vector<D3D12_RESOURCE_BARRIER> m_pending;
};

} // namespace rtsky::render
