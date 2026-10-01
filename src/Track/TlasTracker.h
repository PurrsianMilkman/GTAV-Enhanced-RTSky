// RTSky - capture of the game's top-level acceleration structure (TLAS)
//
// Every top-level BuildRaytracingAccelerationStructure the game records is inspected. The largest
// TLAS (the scene) is cloned, right after its build on the same command list, into one of RTSky's
// own buffers (CopyRaytracingAccelerationStructure CLONE). RTSky's trace later binds the clone:
//   * the game's TLAS memory may be multi-buffered or rebuilt on another queue - the clone ring
//     guarantees that the memory RTSky reads is not rewritten while its dispatch is in flight;
//   * the clone is written in the same frame, before any consumer (the game's own RT passes consume
//     the same build), so the BLASes it references are resident.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>

namespace rtsky::track {

struct ListState;

struct TlasInfo
{
    D3D12_GPU_VIRTUAL_ADDRESS address = 0;   // what RTSky binds (clone or the game's buffer)
    D3D12_GPU_VIRTUAL_ADDRESS gameAddress = 0;
    uint32_t instanceCount = 0;
    uint64_t buildSerial = 0;                // increases with every recorded scene TLAS build
    Microsoft::WRL::ComPtr<ID3D12Resource> cloneBuffer; // keep alive while referenced
};

class TlasTracker
{
public:
    // Called from the BuildRaytracingAccelerationStructure hook AFTER the original call was
    // recorded. May record a clone into `list` (under the caller's hook bypass).
    void OnBuild(ID3D12GraphicsCommandList4* list, ListState& state, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* desc);

    // Latest scene TLAS. Fails if no scene TLAS was built in the last `maxAgeBuilds` scene builds or
    // if nothing was ever captured.
    bool GetSceneTlas(TlasInfo* out) const;

    // Number of top-level builds recorded so far (all sizes), for statistics.
    uint64_t TopLevelBuilds() const;

    // True once the game built an opacity-micromap array or a BLAS with OMM triangles. Tracing
    // against such BLASes without D3D12_RAYTRACING_PIPELINE_FLAG_ALLOW_OPACITY_MICROMAPS is
    // undefined behaviour (DXR 1.2 spec), so the renderer must react.
    bool OpacityMicromapsSeen() const { return m_ommSeen; }

    void SetCloneEnabled(bool enabled) { m_cloneEnabled = enabled; }
    void SetSelect(int select) { m_select = select; }

private:
    static constexpr uint32_t kCloneRing = 4;

    bool EnsureCloneBuffer(ID3D12Device5* device, uint32_t slot, UINT64 size);

    mutable SRWLOCK m_lock = SRWLOCK_INIT;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_clones[kCloneRing];
    UINT64 m_cloneSizes[kCloneRing] = {};
    uint32_t m_nextClone = 0;

    TlasInfo m_latest;
    uint32_t m_maxInstancesRecent = 0;
    uint64_t m_topLevelBuilds = 0;
    uint64_t m_sceneBuilds = 0;
    D3D12_GPU_VIRTUAL_ADDRESS m_distinct[8] = {};
    uint32_t m_distinctCount = 0;
    bool m_cloneEnabled = true;
    volatile bool m_ommSeen = false;
    int m_select = -1;
};

TlasTracker& Tlas();

} // namespace rtsky::track
