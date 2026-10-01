// RTSky - CPU descriptor handle -> view information
#include "DescriptorTracker.h"

#include "../Common/D3D12Compat.h"

#include <windows.h>

#include <unordered_map>

namespace rtsky::track {

struct DescriptorTracker::Shard
{
    mutable SRWLOCK lock = SRWLOCK_INIT;
    std::unordered_map<SIZE_T, ViewInfo> map;
};

DescriptorTracker::DescriptorTracker()
{
    for (Shard*& s : m_shards)
        s = new Shard();
}

DescriptorTracker::~DescriptorTracker()
{
    for (Shard*& s : m_shards)
    {
        delete s;
        s = nullptr;
    }
}

DescriptorTracker::Shard* DescriptorTracker::ShardFor(SIZE_T ptr) const
{
    // Descriptor handles are multiples of the (small) increment size; mix the bits.
    uint64_t h = static_cast<uint64_t>(ptr) * 0x9E3779B97F4A7C15ull;
    return m_shards[(h >> 60) & (kShards - 1)];
}

static void FillResourceInfo(ID3D12Resource* resource, ViewInfo& info)
{
    info.resource = resource;
    if (resource == nullptr)
        return;
    const D3D12_RESOURCE_DESC desc = ResourceDesc(resource);
    info.resourceFormat = desc.Format;
    info.width = desc.Width;
    info.height = desc.Height;
    info.depthOrArraySize = desc.DepthOrArraySize;
    info.mipLevels = desc.MipLevels;
    info.sampleCount = desc.SampleDesc.Count;
    info.resourceFlags = desc.Flags;
}

void DescriptorTracker::Store(D3D12_CPU_DESCRIPTOR_HANDLE handle, const ViewInfo& info)
{
    Shard* s = ShardFor(handle.ptr);
    AcquireSRWLockExclusive(&s->lock);
    s->map[handle.ptr] = info;
    ReleaseSRWLockExclusive(&s->lock);
}

void DescriptorTracker::Erase(D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    Shard* s = ShardFor(handle.ptr);
    AcquireSRWLockExclusive(&s->lock);
    s->map.erase(handle.ptr);
    ReleaseSRWLockExclusive(&s->lock);
}

void DescriptorTracker::OnCreateRTV(ID3D12Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    ViewInfo info;
    info.kind = ViewInfo::Kind::RTV;
    FillResourceInfo(resource, info);
    info.viewFormat = desc != nullptr ? desc->Format : info.resourceFormat;
    Store(handle, info);
}

void DescriptorTracker::OnCreateDSV(ID3D12Resource* resource, const D3D12_DEPTH_STENCIL_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    ViewInfo info;
    info.kind = ViewInfo::Kind::DSV;
    FillResourceInfo(resource, info);
    info.viewFormat = desc != nullptr ? desc->Format : info.resourceFormat;
    info.dsvFlags = desc != nullptr ? desc->Flags : D3D12_DSV_FLAG_NONE;
    Store(handle, info);
}

void DescriptorTracker::OnCreateSRV(ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    if (desc != nullptr && desc->ViewDimension == D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE)
    {
        ViewInfo info;
        info.kind = ViewInfo::Kind::SRV_AS;
        info.asLocation = desc->RaytracingAccelerationStructure.Location;
        Store(handle, info);
        return;
    }
    // A regular SRV overwrote a slot that may have held a tracked view: forget it. Only erase when
    // present to keep this hot path cheap (the vast majority of SRVs are never tracked).
    Shard* s = ShardFor(handle.ptr);
    AcquireSRWLockShared(&s->lock);
    bool present = s->map.find(handle.ptr) != s->map.end();
    ReleaseSRWLockShared(&s->lock);
    if (present)
        Erase(handle);
}

bool DescriptorTracker::Lookup(D3D12_CPU_DESCRIPTOR_HANDLE handle, ViewInfo* out) const
{
    Shard* s = ShardFor(handle.ptr);
    AcquireSRWLockShared(&s->lock);
    auto it = s->map.find(handle.ptr);
    bool found = it != s->map.end();
    if (found && out != nullptr)
        *out = it->second;
    ReleaseSRWLockShared(&s->lock);
    return found;
}

size_t DescriptorTracker::Size() const
{
    size_t total = 0;
    for (Shard* s : m_shards)
    {
        AcquireSRWLockShared(&s->lock);
        total += s->map.size();
        ReleaseSRWLockShared(&s->lock);
    }
    return total;
}

DescriptorTracker& Descriptors()
{
    static DescriptorTracker instance;
    return instance;
}

} // namespace rtsky::track
