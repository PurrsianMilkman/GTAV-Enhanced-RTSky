// RTSky - lifetime of objects referenced by injected commands
#include "GpuLifetime.h"

#include "../Common/Log.h"
#include "../Common/D3D12Compat.h"
#include "../Hooks/Bypass.h"
#include "../Track/CommandListTracker.h"

#include <iterator>

namespace rtsky::render {

using Microsoft::WRL::ComPtr;

void GpuLifetime::Attach(track::ListState& state, std::shared_ptr<void> object)
{
    if (object)
        state.attachments.push_back(std::move(object));
}

void GpuLifetime::AttachBusy(track::ListState& state, std::shared_ptr<void> token)
{
    if (token)
        state.busyTokens.push_back(std::move(token));
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
    ComPtr<ID3D12Device> device;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))))
        return nullptr;
    for (auto& t : m_timelines)
    {
        if (t->queue == queue && t->device == device.Get())
            return t.get();
    }
    auto timeline = std::make_unique<QueueTimeline>();
    timeline->queue = queue;
    timeline->device = device.Get();
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

bool GpuLifetime::OnExecuted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists,
                             ComPtr<ID3D12Fence>* signalledFence, uint64_t* signalledValue)
{
    std::vector<std::shared_ptr<void>> batch;
    bool needSignal = false;
    for (UINT i = 0; i < count; ++i)
    {
        track::ListState* s = track::FindListState(lists[i]);
        if (s == nullptr)
            continue;
        // A list that produced a scene TLAS needs a fence value even without attachments (clone
        // disabled): consumers on other queues wait for it.
        needSignal = needSignal || s->tlasProducedValid;
        if (s->executed && (s->injectedPrepare || s->injectedComposite || s->tlasProducedValid))
        {
            // Memory stays valid (attachments), but the ring slots this list uses were released after
            // its first execution and may already hold newer data.
            RTSKY_LOG_ONCE(log::Level::Warning, "The game re-executes a command list that carries RTSky work; "
                           "its ring slots may have been reused (please report this)");
        }
        s->executed = true;
        // Copy (not move): a closed list may legally be executed more than once.
        batch.insert(batch.end(), s->attachments.begin(), s->attachments.end());
        // Busy tokens only cover the first execution (see ListState::busyTokens), and so do the
        // TlasInfo copies, whose holders would otherwise pin the clone slots in the same way.
        batch.insert(batch.end(), std::make_move_iterator(s->busyTokens.begin()), std::make_move_iterator(s->busyTokens.end()));
        s->busyTokens.clear();
        s->tlasProduced.cloneHolder.reset();
        s->tlasConsumed.cloneHolder.reset();
    }
    if (batch.empty() && !needSignal)
        return false;

    AcquireSRWLockExclusive(&m_lock);
    QueueTimeline* t = TimelineFor(queue);
    if (t == nullptr)
    {
        ReleaseSRWLockExclusive(&m_lock);
        // Without a fence we cannot know when the GPU is done: leak rather than free early.
        RTSKY_LOG_ONCE(log::Level::Error, "GpuLifetime: no fence, leaking attachments");
        for (auto& p : batch)
            (void)new std::shared_ptr<void>(p);
        return false;
    }
    const uint64_t value = ++t->lastSignaled;
    {
        hooks::HookBypass bypass;
        queue->Signal(t->fence.Get(), value);
    }
    t->pending.emplace_back(value, std::move(batch));
    if (signalledFence != nullptr)
        *signalledFence = t->fence;
    if (signalledValue != nullptr)
        *signalledValue = value;
    ReleaseSRWLockExclusive(&m_lock);
    return true;
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
    // Never destroyed: hooks and GPU-lifetime deleters may still run during process exit, after
    // static destructors (destruction order across translation units is unspecified).
    static GpuLifetime* instance = new GpuLifetime();
    return *instance;
}

} // namespace rtsky::render
