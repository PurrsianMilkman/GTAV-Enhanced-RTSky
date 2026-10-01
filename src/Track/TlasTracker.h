// RTSky - capture of the game's top-level acceleration structure (TLAS)
//
// Every top-level BuildRaytracingAccelerationStructure the game records is inspected. The largest
// TLAS (the scene) is cloned, right after its build on the same command list, into one of RTSky's
// own buffers (CopyRaytracingAccelerationStructure CLONE). RTSky's trace later binds the clone:
//   * the game's TLAS memory may be multi-buffered or rebuilt on another queue - a clone buffer is
//     only rewritten once no list that reads it can still be in flight (busy tokens);
//   * a clone becomes visible to other lists only when the list that wrote it is SUBMITTED
//     (Publish, from the ExecuteCommandLists hook). Recording order says nothing about GPU order.
//     A clone recorded earlier in the same list is ordered by the list itself and is preferred.
//     Otherwise a clone from another queue is only used once its producer has completed (no GPU
//     waits between queues are ever needed, so none can deadlock with the game's own waits).
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>

namespace rtsky::track {

struct ListState;

struct TlasInfo
{
    D3D12_GPU_VIRTUAL_ADDRESS address = 0;   // what RTSky binds (clone or the game's buffer)
    D3D12_GPU_VIRTUAL_ADDRESS gameAddress = 0;
    uint32_t instanceCount = 0;
    uint64_t buildSerial = 0;                // increases with every recorded scene TLAS build
    // Keeps the clone buffer alive and marks its ring slot busy while any copy of this exists.
    std::shared_ptr<void> cloneHolder;
    int cloneSlot = -1;     // ring slot of the clone (-1: no clone)
    uint64_t cloneGen = 0;  // the slot's generation when the clone was written
    // Submission of the producing list (set by Publish; null queue = not submitted yet).
    ID3D12CommandQueue* producerQueue = nullptr; // not AddRef'd, compared only
    Microsoft::WRL::ComPtr<ID3D12Fence> producerFence;
    uint64_t producerFenceValue = 0;
};

class TlasTracker
{
public:
    // Called from the BuildRaytracingAccelerationStructure hook AFTER the original call was
    // recorded. May record a clone into `list` (under the caller's hook bypass). The result is kept
    // in the list's state (ListState::tlasProduced) until the list is submitted.
    void OnBuild(ID3D12GraphicsCommandList4* list, ListState& state, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* desc);

    // Called after a list carrying a scene TLAS was submitted on `queue`; `fence` reaches `value`
    // once that submission has completed on the GPU.
    void Publish(const TlasInfo& produced, ID3D12CommandQueue* queue, ID3D12Fence* fence, uint64_t value);

    // Newest SUBMITTED scene TLAS that a consumer may read without a cross-queue wait: one whose
    // producing submission has completed, or one produced on the queue that runs RTSky's composite
    // (NoteConsumerQueue). The last few published clones are kept, so a producer on another queue
    // that is always a frame or two ahead still yields a fresh, completed clone each frame. Fails if
    // there is none.
    bool GetSceneTlas(TlasInfo* out);

    // The queue on which lists carrying RTSky's composite were last submitted.
    void NoteConsumerQueue(ID3D12CommandQueue* queue);

    // True when the producing submission of `info` has completed on the GPU (or was never tracked).
    static bool ProducerDone(const TlasInfo& info);

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

    // Picks a ring slot that no in-flight or pending list references, (re)allocating its buffer
    // when it is too small or when every slot is busy. Returns the slot or -1.
    int AcquireCloneSlot(ID3D12Device5* device, UINT64 size);

    mutable SRWLOCK m_lock = SRWLOCK_INIT;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_clones[kCloneRing];
    std::shared_ptr<void> m_cloneHolders[kCloneRing]; // use_count() == 1: only the ring holds it
    uint64_t m_slotGen[kCloneRing] = {};              // bumped whenever a slot is (re)written
    UINT64 m_cloneSizes[kCloneRing] = {};
    uint32_t m_nextClone = 0;

    // Published clones, newest first. They do not hold the busy token (that would pin ring slots);
    // a slot that was rewritten since is detected through its generation.
    static constexpr uint32_t kHistory = 4;
    TlasInfo m_history[kHistory];
    uint32_t m_historyCount = 0;
    ID3D12CommandQueue* m_consumerQueue = nullptr; // not AddRef'd, compared only
    ID3D12Device* m_device = nullptr;              // device of the clone ring (not AddRef'd)
    uint64_t m_allocRetryAt = 0;                   // scene-build count before which allocation is not retried
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
