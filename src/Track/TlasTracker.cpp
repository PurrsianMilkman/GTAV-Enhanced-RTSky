// RTSky - capture of the game's top-level acceleration structure
#include "TlasTracker.h"
#include "CommandListTracker.h"

#include "../Common/Log.h"
#include "../Common/D3D12Compat.h"
#include "../Render/GpuLifetime.h"

namespace rtsky::track {

using Microsoft::WRL::ComPtr;

bool TlasTracker::EnsureCloneBuffer(ID3D12Device5* device, uint32_t slot, UINT64 size)
{
    if (m_clones[slot] && m_cloneSizes[slot] >= size)
        return true;

    // Grow with headroom so that instance-count jitter does not reallocate every frame.
    UINT64 allocSize = (size + size / 4 + 65535) & ~UINT64(65535);

    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = allocSize;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ComPtr<ID3D12Resource> buffer;
    HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                 D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr,
                                                 IID_PPV_ARGS(&buffer));
    if (FAILED(hr))
    {
        LOG_ERROR("TLAS clone buffer allocation (%llu bytes) failed: 0x%08X", static_cast<unsigned long long>(allocSize),
                  static_cast<unsigned>(hr));
        return false;
    }
    buffer->SetName(L"RTSky TLAS clone");
    // The previous buffer stays alive through the references held by in-flight lists / slots.
    m_clones[slot] = buffer;
    m_cloneSizes[slot] = allocSize;
    LOG_INFO("TLAS clone buffer %u: %llu bytes", slot, static_cast<unsigned long long>(allocSize));
    return true;
}

void TlasTracker::OnBuild(ID3D12GraphicsCommandList4* list, ListState& state,
                          const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* desc)
{
    if (desc == nullptr)
        return;
    if (desc->Inputs.Type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_ARRAY)
    {
        if (!m_ommSeen)
            LOG_WARN("The game builds opacity micromaps (DXR 1.2)");
        m_ommSeen = true;
        return;
    }
    if (desc->Inputs.Type != D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL)
    {
        ++state.blasBuilds;
        if (!m_ommSeen && desc->Inputs.Type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL)
        {
            for (UINT i = 0; i < desc->Inputs.NumDescs; ++i)
            {
                const D3D12_RAYTRACING_GEOMETRY_DESC* g = desc->Inputs.DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY
                    ? &desc->Inputs.pGeometryDescs[i]
                    : desc->Inputs.ppGeometryDescs[i];
                if (g != nullptr && g->Type == D3D12_RAYTRACING_GEOMETRY_TYPE_OMM_TRIANGLES)
                {
                    LOG_WARN("The game builds BLASes with opacity micromaps (DXR 1.2)");
                    m_ommSeen = true;
                    break;
                }
            }
        }
        return;
    }
    ++state.tlasBuilds;

    const uint32_t instances = desc->Inputs.NumDescs;
    const D3D12_GPU_VIRTUAL_ADDRESS dest = desc->DestAccelerationStructureData;
    if (dest == 0 || instances == 0)
        return;

    AcquireSRWLockExclusive(&m_lock);
    ++m_topLevelBuilds;

    // Remember distinct destinations (for TlasSelect=<n> and the frame dump)
    bool known = false;
    uint32_t distinctIndex = 0;
    for (uint32_t i = 0; i < m_distinctCount; ++i)
    {
        if (m_distinct[i] == dest)
        {
            known = true;
            distinctIndex = i;
        }
    }
    if (!known && m_distinctCount < 8)
    {
        distinctIndex = m_distinctCount;
        m_distinct[m_distinctCount++] = dest;
    }

    // Scene TLAS selection: the largest one (slowly decaying maximum), or the configured one.
    m_maxInstancesRecent = m_maxInstancesRecent > instances ? m_maxInstancesRecent - (m_maxInstancesRecent >> 7) : instances;
    bool isScene = m_select >= 0 ? (distinctIndex == static_cast<uint32_t>(m_select))
                                 : (instances * 2 >= m_maxInstancesRecent);
    if (!isScene)
    {
        ReleaseSRWLockExclusive(&m_lock);
        return;
    }

    TlasInfo info;
    info.gameAddress = dest;
    info.address = dest;
    info.instanceCount = instances;
    info.buildSerial = ++m_sceneBuilds;

    if (m_cloneEnabled)
    {
        ComPtr<ID3D12Device5> device;
        if (SUCCEEDED(list->GetDevice(IID_PPV_ARGS(&device))))
        {
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild = {};
            device->GetRaytracingAccelerationStructurePrebuildInfo(&desc->Inputs, &prebuild);
            const uint32_t slot = m_nextClone;
            if (prebuild.ResultDataMaxSizeInBytes > 0 && EnsureCloneBuffer(device.Get(), slot, prebuild.ResultDataMaxSizeInBytes))
            {
                m_nextClone = (m_nextClone + 1) % kCloneRing;

                // build -> (UAV barrier) -> clone -> (UAV barrier): the game may rebuild or update the
                // same destination later in this list, which must not overlap our read.
                D3D12_RESOURCE_BARRIER uav = {};
                uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                uav.UAV.pResource = nullptr;
                list->ResourceBarrier(1, &uav);
                const D3D12_GPU_VIRTUAL_ADDRESS cloneAddress = m_clones[slot]->GetGPUVirtualAddress();
                list->CopyRaytracingAccelerationStructure(cloneAddress, dest, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_CLONE);
                list->ResourceBarrier(1, &uav);

                render::Lifetime().Attach(state, m_clones[slot]);
                info.address = cloneAddress;
                info.cloneBuffer = m_clones[slot];
            }
        }
    }

    m_latest = info;
    ReleaseSRWLockExclusive(&m_lock);
}

bool TlasTracker::GetSceneTlas(TlasInfo* out) const
{
    AcquireSRWLockShared(&m_lock);
    bool ok = m_latest.address != 0;
    if (ok)
        *out = m_latest;
    ReleaseSRWLockShared(&m_lock);
    return ok;
}

uint64_t TlasTracker::TopLevelBuilds() const
{
    AcquireSRWLockShared(&m_lock);
    uint64_t n = m_topLevelBuilds;
    ReleaseSRWLockShared(&m_lock);
    return n;
}

TlasTracker& Tlas()
{
    static TlasTracker instance;
    return instance;
}

} // namespace rtsky::track
