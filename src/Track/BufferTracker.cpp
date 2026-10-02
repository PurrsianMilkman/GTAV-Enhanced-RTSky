// RTSky - GPU virtual address -> buffer
#include "BufferTracker.h"

#include "../Common/D3D12Compat.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <iterator>
#include <map>

namespace rtsky::track {
namespace {

SRWLOCK g_lock = SRWLOCK_INIT;
// base VA -> info. Never destroyed (resources can be released during process exit).
std::map<D3D12_GPU_VIRTUAL_ADDRESS, BufferInfo>* g_buffers = new std::map<D3D12_GPU_VIRTUAL_ADDRESS, BufferInfo>();
std::atomic<uint64_t> g_tracked{ 0 };

const GUID kBufferWatchGuid = { 0x7b2d9e41, 0x5c3a, 0x4f18, { 0xa6, 0x0e, 0x91, 0x4c, 0x2b, 0x77, 0xd5, 0x3f } };

// Released by the runtime when the buffer is destroyed: its address range may be reused after that.
class BufferDeathWatch final : public IUnknown
{
public:
    BufferDeathWatch(ID3D12Resource* resource, D3D12_GPU_VIRTUAL_ADDRESS base) : m_resource(resource), m_base(base) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (out == nullptr)
            return E_POINTER;
        if (riid == __uuidof(IUnknown))
        {
            *out = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return m_refs.fetch_add(1) + 1; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG left = m_refs.fetch_sub(1) - 1;
        if (left == 0)
        {
            AcquireSRWLockExclusive(&g_lock);
            auto it = g_buffers->find(m_base);
            if (it != g_buffers->end() && it->second.resource == m_resource)
            {
                g_buffers->erase(it);
                g_tracked.fetch_sub(1, std::memory_order_relaxed);
            }
            ReleaseSRWLockExclusive(&g_lock);
            delete this;
        }
        return left;
    }

private:
    std::atomic<ULONG> m_refs{ 1 };
    ID3D12Resource* m_resource;
    D3D12_GPU_VIRTUAL_ADDRESS m_base;
};

bool CpuReadable(const BufferInfo& b)
{
    return b.heapType == D3D12_HEAP_TYPE_UPLOAD || b.heapType == D3D12_HEAP_TYPE_READBACK ||
           (b.heapType == D3D12_HEAP_TYPE_CUSTOM &&
            (b.cpuPage == D3D12_CPU_PAGE_PROPERTY_WRITE_BACK || b.cpuPage == D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE));
}

} // namespace

void NoteBuffer(ID3D12Resource* resource, D3D12_HEAP_TYPE heapType, D3D12_CPU_PAGE_PROPERTY cpuPage)
{
    if (resource == nullptr)
        return;
    const D3D12_RESOURCE_DESC desc = ResourceDesc(resource);
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER)
        return;
    BufferInfo info;
    info.resource = resource;
    info.base = resource->GetGPUVirtualAddress();
    info.size = desc.Width;
    info.heapType = heapType;
    info.cpuPage = cpuPage;
    if (info.base == 0)
        return;
    AcquireSRWLockExclusive(&g_lock);
    const bool fresh = g_buffers->find(info.base) == g_buffers->end();
    (*g_buffers)[info.base] = info;
    ReleaseSRWLockExclusive(&g_lock);
    if (fresh)
        g_tracked.fetch_add(1, std::memory_order_relaxed);
    BufferDeathWatch* watch = new BufferDeathWatch(resource, info.base);
    resource->SetPrivateDataInterface(kBufferWatchGuid, watch);
    watch->Release();
}

bool FindBuffer(D3D12_GPU_VIRTUAL_ADDRESS va, BufferInfo* out)
{
    AcquireSRWLockShared(&g_lock);
    bool found = false;
    auto it = g_buffers->upper_bound(va);
    if (it != g_buffers->begin())
    {
        --it;
        if (va >= it->second.base && va - it->second.base < it->second.size)
        {
            *out = it->second;
            found = true;
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return found;
}

uint64_t BuffersTracked()
{
    return g_tracked.load(std::memory_order_relaxed);
}

bool ReadBufferCpu(D3D12_GPU_VIRTUAL_ADDRESS va, void* out, size_t size, BufferInfo* info)
{
    // Only called with an address the game has bound for a draw it is recording, so the buffer is
    // alive (the game must keep it until the GPU has executed that draw).
    BufferInfo b;
    if (!FindBuffer(va, &b))
        return false;
    if (info != nullptr)
        *info = b;
    const UINT64 offset = va - b.base;
    if (!CpuReadable(b) || offset + size > b.size)
        return false;
    const D3D12_RANGE read = { static_cast<SIZE_T>(offset), static_cast<SIZE_T>(offset + size) };
    void* p = nullptr;
    if (FAILED(b.resource->Map(0, &read, &p)) || p == nullptr)
        return false;
    std::memcpy(out, static_cast<const uint8_t*>(p) + offset, size);
    const D3D12_RANGE none = { 0, 0 };
    b.resource->Unmap(0, &none);
    return true;
}

} // namespace rtsky::track
