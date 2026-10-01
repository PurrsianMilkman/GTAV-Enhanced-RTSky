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
};

D3D12_RESOURCE_STATES LegacyStateFor(Usage u);
D3D12_BARRIER_LAYOUT LayoutFor(Usage u);
D3D12_BARRIER_SYNC SyncFor(Usage u);
D3D12_BARRIER_ACCESS AccessFor(Usage u);
// Converts an observed layout back to a usage (best effort, for layouts RTSky knows how to leave).
bool UsageFromLayout(D3D12_BARRIER_LAYOUT layout, Usage* out);
bool UsageFromLegacyState(D3D12_RESOURCE_STATES state, Usage* out);

// Transition of a game resource between two states. `subresource` is a subresource index for legacy
// barriers, or the plane (0 = depth) for enhanced barriers when planeOnly is true.
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
