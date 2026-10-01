// RTSky - barriers
#include "Barriers.h"
#include "../Common/D3D12Compat.h"

#include <wrl/client.h>

namespace rtsky::render {

D3D12_RESOURCE_STATES LegacyStateFor(Usage u)
{
    switch (u)
    {
    case Usage::RenderTarget: return D3D12_RESOURCE_STATE_RENDER_TARGET;
    case Usage::DepthWrite: return D3D12_RESOURCE_STATE_DEPTH_WRITE;
    case Usage::DepthRead: return D3D12_RESOURCE_STATE_DEPTH_READ;
    case Usage::ComputeRead: return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    case Usage::ComputeWrite: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    case Usage::CopySource: return D3D12_RESOURCE_STATE_COPY_SOURCE;
    case Usage::CopyDest: return D3D12_RESOURCE_STATE_COPY_DEST;
    case Usage::Common: return D3D12_RESOURCE_STATE_COMMON;
    }
    return D3D12_RESOURCE_STATE_COMMON;
}

D3D12_BARRIER_LAYOUT LayoutFor(Usage u)
{
    switch (u)
    {
    case Usage::RenderTarget: return D3D12_BARRIER_LAYOUT_RENDER_TARGET;
    case Usage::DepthWrite: return D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE;
    case Usage::DepthRead: return D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ;
    case Usage::ComputeRead: return D3D12_BARRIER_LAYOUT_SHADER_RESOURCE;
    case Usage::ComputeWrite: return D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS;
    case Usage::CopySource: return D3D12_BARRIER_LAYOUT_COPY_SOURCE;
    case Usage::CopyDest: return D3D12_BARRIER_LAYOUT_COPY_DEST;
    case Usage::Common: return D3D12_BARRIER_LAYOUT_COMMON;
    }
    return D3D12_BARRIER_LAYOUT_COMMON;
}

D3D12_BARRIER_SYNC SyncFor(Usage u)
{
    switch (u)
    {
    case Usage::RenderTarget: return D3D12_BARRIER_SYNC_RENDER_TARGET;
    case Usage::DepthWrite:
    case Usage::DepthRead: return D3D12_BARRIER_SYNC_DEPTH_STENCIL;
    case Usage::ComputeRead:
    case Usage::ComputeWrite: return D3D12_BARRIER_SYNC_COMPUTE_SHADING;
    case Usage::CopySource:
    case Usage::CopyDest: return D3D12_BARRIER_SYNC_COPY;
    case Usage::Common: return D3D12_BARRIER_SYNC_ALL;
    }
    return D3D12_BARRIER_SYNC_ALL;
}

D3D12_BARRIER_ACCESS AccessFor(Usage u)
{
    switch (u)
    {
    case Usage::RenderTarget: return D3D12_BARRIER_ACCESS_RENDER_TARGET;
    case Usage::DepthWrite: return D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE;
    case Usage::DepthRead: return D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ;
    case Usage::ComputeRead: return D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
    case Usage::ComputeWrite: return D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
    case Usage::CopySource: return D3D12_BARRIER_ACCESS_COPY_SOURCE;
    case Usage::CopyDest: return D3D12_BARRIER_ACCESS_COPY_DEST;
    case Usage::Common: return D3D12_BARRIER_ACCESS_COMMON;
    }
    return D3D12_BARRIER_ACCESS_COMMON;
}

bool UsageFromLayout(D3D12_BARRIER_LAYOUT layout, Usage* out)
{
    switch (layout)
    {
    case D3D12_BARRIER_LAYOUT_RENDER_TARGET: *out = Usage::RenderTarget; return true;
    case D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE: *out = Usage::DepthWrite; return true;
    case D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ: *out = Usage::DepthRead; return true;
    case D3D12_BARRIER_LAYOUT_SHADER_RESOURCE:
    case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE: *out = Usage::ComputeRead; return true;
    case D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS:
    case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS: *out = Usage::ComputeWrite; return true;
    case D3D12_BARRIER_LAYOUT_COPY_SOURCE:
    case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_COPY_SOURCE: *out = Usage::CopySource; return true;
    case D3D12_BARRIER_LAYOUT_COPY_DEST:
    case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_COPY_DEST: *out = Usage::CopyDest; return true;
    default: return false;
    }
}

bool UsageFromLegacyState(D3D12_RESOURCE_STATES state, Usage* out)
{
    // Only exact single-purpose states are mapped; combined read states keep their exact bits in
    // GameResourceState::legacyState and are restored verbatim.
    if (state == D3D12_RESOURCE_STATE_RENDER_TARGET) { *out = Usage::RenderTarget; return true; }
    if (state == D3D12_RESOURCE_STATE_DEPTH_WRITE) { *out = Usage::DepthWrite; return true; }
    if ((state & D3D12_RESOURCE_STATE_DEPTH_READ) != 0 && (state & ~(D3D12_RESOURCE_STATE_DEPTH_READ |
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)) == 0)
    {
        *out = Usage::DepthRead;
        return true;
    }
    if (state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) { *out = Usage::ComputeWrite; return true; }
    if (state == D3D12_RESOURCE_STATE_COPY_SOURCE) { *out = Usage::CopySource; return true; }
    if (state == D3D12_RESOURCE_STATE_COPY_DEST) { *out = Usage::CopyDest; return true; }
    if ((state & ~(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)) == 0 &&
        (state & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) != 0)
    {
        *out = Usage::ComputeRead;
        return true;
    }
    return false;
}

static void IssueEnhanced(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_BARRIER_LAYOUT layoutBefore,
                          D3D12_BARRIER_SYNC syncBefore, D3D12_BARRIER_ACCESS accessBefore, D3D12_BARRIER_LAYOUT layoutAfter,
                          D3D12_BARRIER_SYNC syncAfter, D3D12_BARRIER_ACCESS accessAfter, bool depthPlaneOnly)
{
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList7> list7;
    if (FAILED(list->QueryInterface(IID_PPV_ARGS(&list7))))
        return;
    D3D12_TEXTURE_BARRIER tb = {};
    tb.SyncBefore = syncBefore;
    tb.SyncAfter = syncAfter;
    tb.AccessBefore = accessBefore;
    tb.AccessAfter = accessAfter;
    tb.LayoutBefore = layoutBefore;
    tb.LayoutAfter = layoutAfter;
    tb.pResource = resource;
    if (depthPlaneOnly)
    {
        tb.Subresources.IndexOrFirstMipLevel = 0;
        tb.Subresources.NumMipLevels = 1;
        tb.Subresources.FirstArraySlice = 0;
        tb.Subresources.NumArraySlices = 1;
        tb.Subresources.FirstPlane = 0;
        tb.Subresources.NumPlanes = 1;
    }
    else
    {
        tb.Subresources.IndexOrFirstMipLevel = 0xFFFFFFFFu; // all subresources
        tb.Subresources.NumMipLevels = 0;
    }
    tb.Flags = D3D12_TEXTURE_BARRIER_FLAG_NONE;
    D3D12_BARRIER_GROUP group = {};
    group.Type = D3D12_BARRIER_TYPE_TEXTURE;
    group.NumBarriers = 1;
    group.pTextureBarriers = &tb;
    list7->Barrier(1, &group);
}

static void IssueLegacy(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                        D3D12_RESOURCE_STATES after, bool depthPlaneOnly)
{
    if (before == after)
        return;
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = depthPlaneOnly ? 0 : D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    list->ResourceBarrier(1, &b);
}

void TransitionGame(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, const GameResourceState& from, Usage to,
                    bool depthPlaneOnly)
{
    if (from.enhanced)
    {
        IssueEnhanced(list, resource, from.layout, SyncFor(from.usage), AccessFor(from.usage), LayoutFor(to), SyncFor(to),
                      AccessFor(to), depthPlaneOnly);
    }
    else
    {
        IssueLegacy(list, resource, from.legacyState, LegacyStateFor(to), depthPlaneOnly);
    }
}

void RestoreGame(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, Usage current, const GameResourceState& original,
                 bool depthPlaneOnly)
{
    if (original.enhanced)
    {
        IssueEnhanced(list, resource, LayoutFor(current), SyncFor(current), AccessFor(current), original.layout,
                      SyncFor(original.usage), AccessFor(original.usage), depthPlaneOnly);
    }
    else
    {
        IssueLegacy(list, resource, LegacyStateFor(current), original.legacyState, depthPlaneOnly);
    }
}

void OwnStates::Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    if (resource == nullptr || before == after)
        return;
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    m_pending.push_back(b);
}

void OwnStates::Uav(ID3D12Resource* resource)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = resource;
    m_pending.push_back(b);
}

void OwnStates::Flush(ID3D12GraphicsCommandList* list)
{
    if (m_pending.empty())
        return;
    list->ResourceBarrier(static_cast<UINT>(m_pending.size()), m_pending.data());
    m_pending.clear();
}

} // namespace rtsky::render
