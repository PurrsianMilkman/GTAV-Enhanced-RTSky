// RTSky - lifetime of objects referenced by commands RTSky records into the game's command lists
//
// RTSky never submits command lists of its own. Everything it records goes into the game's lists,
// so it cannot wait on "its" work. Instead:
//   * objects an injected command references are attached to the game's list (Attach);
//   * when the game submits that list (ExecuteCommandLists hook), a copy of the attachments is
//     queued with a fresh value of RTSky's fence, which is signalled on the same queue right after
//     the game's submission;
//   * queued attachments are dropped once the fence has passed that value (Collect).
// Attachments are shared_ptr<void>, so any resource, ring slot or callback can be kept alive.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

namespace rtsky::track {
struct ListState;
}

namespace rtsky::render {

class GpuLifetime
{
public:
    void Attach(track::ListState& state, std::shared_ptr<void> object);
    void Attach(track::ListState& state, const Microsoft::WRL::ComPtr<ID3D12Resource>& resource);
    void Attach(track::ListState& state, const Microsoft::WRL::ComPtr<ID3D12DeviceChild>& object);
    // A ring slot's busy token: held only until the GPU has finished the list's FIRST execution
    // (see ListState::busyTokens). Whatever memory the slot refers to must also be Attach'ed.
    void AttachBusy(track::ListState& state, std::shared_ptr<void> token);

    // After the game's ExecuteCommandLists was forwarded. Signals the fence on `queue` when any of
    // the lists carries attachments; returns true and the signalled fence / value in that case.
    bool OnExecuted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists,
                    Microsoft::WRL::ComPtr<ID3D12Fence>* signalledFence = nullptr, uint64_t* signalledValue = nullptr);

    // Releases everything whose fence value has completed. Cheap when there is nothing to do.
    void Collect();

    // Number of attachment batches still waiting for the GPU (statistics).
    size_t PendingBatches() const;

private:
    // One fence per queue: signals on different queues complete in any order.
    struct QueueTimeline
    {
        ID3D12CommandQueue* queue = nullptr; // not AddRef'd, only used as a key (with the device:
        ID3D12Device* device = nullptr;      // a recreated device can reuse a queue address)
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        uint64_t lastSignaled = 0;
        std::deque<std::pair<uint64_t, std::vector<std::shared_ptr<void>>>> pending;
    };
    QueueTimeline* TimelineFor(ID3D12CommandQueue* queue);

    mutable SRWLOCK m_lock = SRWLOCK_INIT;
    std::vector<std::unique_ptr<QueueTimeline>> m_timelines;
};

GpuLifetime& Lifetime();

// Wraps a COM object in a shared_ptr<void> that releases it.
template <typename T>
std::shared_ptr<void> KeepAlive(T* object)
{
    if (object == nullptr)
        return {};
    object->AddRef();
    return std::shared_ptr<void>(static_cast<void*>(object), [object](void*) { object->Release(); });
}

} // namespace rtsky::render
