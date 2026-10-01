// RTSky - lifetime of objects referenced by injected commands
#include "GpuLifetime.h"

#include "../Common/Log.h"
#include "../Hooks/Bypass.h"
#include "../Track/CommandListTracker.h"

namespace rtsky::render {

using Microsoft::WRL::ComPtr;

void GpuLifetime::Attach(track::ListState& state, std::shared_ptr<void> object)
{
    if (object)
        state.attachments.push_back(std::move(object));
}

void GpuLifetime::Attach(track::ListState& state, const ComPtr<ID3D12Resource>& resource)
{
    Attach(state, KeepAlive(resource.Get()));
}

void GpuLifetime::Attach(track::ListState& state, const ComPtr<ID3D12DeviceChild>& object)
{
    Attach(state, KeepAlive(object.Get()));
}

GpuLifetime::QueueTimeline* GpuLifetime::TimelineFor(ID3D12CommandQueue* queue)
{
    for (auto& t : m_timelines)
    {
        if (t->queue == queue)
            return t.get();
    }
    ComPtr<ID3D12Device> device;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))))
        return nullptr;
    auto timeline = std::make_unique<QueueTimeline>();
    timeline->queue = queue;
    {
        hooks::HookBypass bypass;
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&timeline->fence))))
        {
            LOG_ERROR("GpuLifetime: CreateFence failed");
            return nullptr;
        }
    }
    timeline->fence->SetName(L"RTSky lifetime fence");
    m_timelines.push_back(std::move(timeline));
    LOG_INFO("GpuLifetime: tracking queue %p", static_cast<void*>(queue));
    return m_timelines.back().get();
}

void GpuLifetime::OnExecuted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    std::vector<std::shared_ptr<void>> batch;
    for (UINT i = 0; i < count; ++i)
    {
        track::ListState* s = track::FindListState(lists[i]);
        if (s == nullptr || s->attachments.empty())
            continue;
        // Copy (not move): a closed list may legally be executed more than once.
        batch.insert(batch.end(), s->attachments.begin(), s->attachments.end());
    }
    if (batch.empty())
        return;

    AcquireSRWLockExclusive(&m_lock);
    QueueTimeline* t = TimelineFor(queue);
    if (t == nullptr)
    {
        ReleaseSRWLockExclusive(&m_lock);
        // Without a fence we cannot know when the GPU is done: leak rather than free early.
        RTSKY_LOG_ONCE(log::Level::Error, "GpuLifetime: no fence, leaking attachments");
        for (auto& p : batch)
            (void)new std::shared_ptr<void>(p);
        return;
    }
    const uint64_t value = ++t->lastSignaled;
    {
        hooks::HookBypass bypass;
        queue->Signal(t->fence.Get(), value);
    }
    t->pending.emplace_back(value, std::move(batch));
    ReleaseSRWLockExclusive(&m_lock);
}

void GpuLifetime::Collect()
{
    std::vector<std::vector<std::shared_ptr<void>>> done;
    AcquireSRWLockExclusive(&m_lock);
    for (auto& t : m_timelines)
    {
        if (t->pending.empty())
            continue;
        const uint64_t completed = t->fence->GetCompletedValue();
        if (completed == UINT64_MAX)
        {
            // Device removed: nothing will complete, and nothing is in use any more either.
            for (auto& p : t->pending)
                done.push_back(std::move(p.second));
            t->pending.clear();
            continue;
        }
        while (!t->pending.empty() && t->pending.front().first <= completed)
        {
            done.push_back(std::move(t->pending.front().second));
            t->pending.pop_front();
        }
    }
    ReleaseSRWLockExclusive(&m_lock);
    // Destroy outside the lock (destructors may call into D3D12).
    done.clear();
}

size_t GpuLifetime::PendingBatches() const
{
    AcquireSRWLockShared(&m_lock);
    size_t n = 0;
    for (const auto& t : m_timelines)
        n += t->pending.size();
    ReleaseSRWLockShared(&m_lock);
    return n;
}

GpuLifetime& Lifetime()
{
    static GpuLifetime instance;
    return instance;
}

} // namespace rtsky::render
