// RTSky - capture of the game's top-level acceleration structure
#include "TlasTracker.h"
#include "CommandListTracker.h"

#include "../Common/Log.h"
#include "../Common/D3D12Compat.h"
#include "../Render/GpuLifetime.h"

#include <algorithm>

namespace rtsky::track {

using Microsoft::WRL::ComPtr;

int TlasTracker::AcquireCloneSlot(ID3D12Device5* device, UINT64 size)
{
    // A slot is free when nothing but the ring references its holder: every list that wrote or reads
    // the clone keeps a copy of the holder until the GPU has finished with it (GpuLifetime), and the
    // published / pending TlasInfo copies hold one too. Copies are only made from existing
    // references, so use_count() == 1 cannot rise concurrently (the ring is only touched under m_lock).
    int slot = -1;
    for (uint32_t i = 0; i < kCloneRing; ++i)
    {
        const uint32_t k = (m_nextClone + i) % kCloneRing;
        if (!m_cloneHolders[k] || m_cloneHolders[k].use_count() == 1)
        {
            slot = static_cast<int>(k);
            break;
        }
    }
    const bool allBusy = slot < 0;
    if (allBusy)
        slot = static_cast<int>(m_nextClone); // replaced below; the old buffer lives on through its holders
    m_nextClone = (static_cast<uint32_t>(slot) + 1) % kCloneRing;

    if (!allBusy && m_clones[slot] && m_cloneSizes[slot] >= size)
        return slot;

    // Grow with headroom so that instance-count jitter does not reallocate every frame.
    const UINT64 needed = std::max(size, allBusy ? m_cloneSizes[slot] : UINT64(0));
    const UINT64 allocSize = (needed + needed / 4 + 65535) & ~UINT64(65535);

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
        return -1;
    }
    buffer->SetName(L"RTSky TLAS clone");
    m_clones[slot] = buffer;
    m_cloneHolders[slot] = render::KeepAlive(buffer.Get());
    m_cloneSizes[slot] = allocSize;
    LOG_INFO("TLAS clone buffer %d: %llu bytes%s", slot, static_cast<unsigned long long>(allocSize),
             allBusy ? " (all slots in flight)" : "");
    return slot;
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
        bool cloned = false;
        ComPtr<ID3D12Device5> device;
        if (m_sceneBuilds >= m_allocRetryAt && SUCCEEDED(list->GetDevice(IID_PPV_ARGS(&device))))
        {
            if (device.Get() != m_device)
            {
                // A different device (recreated after a device removal): the ring belongs to the old one.
                for (uint32_t i = 0; i < kCloneRing; ++i)
                {
                    m_clones[i].Reset();
                    m_cloneHolders[i].reset();
                    m_cloneSizes[i] = 0;
                }
                m_latest = TlasInfo{};
                m_completed = TlasInfo{};
                m_device = device.Get();
            }
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild = {};
            device->GetRaytracingAccelerationStructurePrebuildInfo(&desc->Inputs, &prebuild);
            const int slot = prebuild.ResultDataMaxSizeInBytes > 0 ? AcquireCloneSlot(device.Get(), prebuild.ResultDataMaxSizeInBytes) : -1;
            if (slot >= 0)
            {
                cloned = true;
                // build -> (UAV barrier) -> clone -> (UAV barrier): the game may rebuild or update the
                // same destination later in this list, which must not overlap our read.
                D3D12_RESOURCE_BARRIER uav = {};
                uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                uav.UAV.pResource = nullptr;
                list->ResourceBarrier(1, &uav);
                const D3D12_GPU_VIRTUAL_ADDRESS cloneAddress = m_clones[slot]->GetGPUVirtualAddress();
                list->CopyRaytracingAccelerationStructure(cloneAddress, dest, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_CLONE);
                list->ResourceBarrier(1, &uav);

                render::Lifetime().Attach(state, m_cloneHolders[slot]);
                info.address = cloneAddress;
                info.cloneHolder = m_cloneHolders[slot];
            }
            else
            {
                // Out of memory (or no size): back off for ~2 s of scene builds instead of retrying
                // a large allocation every frame.
                m_allocRetryAt = m_sceneBuilds + 120;
            }
        }
        if (!cloned)
        {
            // Never fall back to the game's own TLAS memory: it may be rebuilt or reallocated while
            // RTSky's trace reads it. No clone -> no trace this frame.
            ReleaseSRWLockExclusive(&m_lock);
            return;
        }
    }

    // Not visible to other lists before this one is submitted (see Publish).
    state.tlasProduced = info;
    state.tlasProducedValid = true;
    ReleaseSRWLockExclusive(&m_lock);
}

void TlasTracker::Publish(const TlasInfo& produced, ID3D12CommandQueue* queue, ID3D12Fence* fence, uint64_t value)
{
    AcquireSRWLockExclusive(&m_lock);
    // Lists can be submitted out of recording order: never go back to an older build.
    if (produced.buildSerial >= m_latest.buildSerial)
    {
        if (m_latest.address != 0 && ProducerDone(m_latest))
            m_completed = m_latest;
        m_latest = produced;
        m_latest.producerQueue = queue;
        m_latest.producerFence = fence;
        m_latest.producerFenceValue = value;
    }
    ReleaseSRWLockExclusive(&m_lock);
}

bool TlasTracker::ProducerDone(const TlasInfo& info)
{
    // UINT64_MAX: device removed, nothing is running any more.
    return !info.producerFence || info.producerFence->GetCompletedValue() >= info.producerFenceValue;
}

bool TlasTracker::GetSceneTlas(TlasInfo* out)
{
    AcquireSRWLockExclusive(&m_lock);
    bool ok = false;
    if (m_latest.address != 0)
    {
        const bool done = ProducerDone(m_latest);
        if (done)
            m_completed = m_latest;
        // A clone still being produced is only safe on the queue that will run the consumer (same
        // queue = submission order). Waiting on another queue's fence could deadlock with the
        // game's own cross-queue waits, so otherwise the newest COMPLETED clone is used.
        if (done || (m_latest.producerQueue != nullptr && m_latest.producerQueue == m_consumerQueue))
        {
            *out = m_latest;
            ok = true;
        }
    }
    if (!ok && m_completed.address != 0)
    {
        *out = m_completed;
        ok = true;
    }
    ReleaseSRWLockExclusive(&m_lock);
    return ok;
}

void TlasTracker::NoteConsumerQueue(ID3D12CommandQueue* queue)
{
    AcquireSRWLockExclusive(&m_lock);
    m_consumerQueue = queue;
    ReleaseSRWLockExclusive(&m_lock);
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
    // Never destroyed: hooks and GPU-lifetime deleters may still run during process exit, after
    // static destructors (destruction order across translation units is unspecified).
    static TlasTracker* instance = new TlasTracker();
    return *instance;
}

} // namespace rtsky::track
