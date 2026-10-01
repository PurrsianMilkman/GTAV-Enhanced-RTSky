// RTSky - D3D12 hooks
#include "D3D12Hooks.h"

#include "Bypass.h"
#include "VTableHook.h"
#include "VTableIndices.h"

#include "../Common/Log.h"
#include "../Common/D3D12Compat.h"
#include "../Render/GpuLifetime.h"
#include "../Render/Renderer.h"
#include "../Track/CommandListTracker.h"
#include "../Track/DescriptorTracker.h"
#include "../Track/FrameAnalyzer.h"
#include "../Track/TlasTracker.h"

#include <MinHook.h>
#include <dxgi1_6.h>
#include <winternl.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>

namespace rtsky::hooks {
namespace {

using Microsoft::WRL::ComPtr;
using track::ListState;

VTableHook g_device("ID3D12Device");
VTableHook g_list("ID3D12GraphicsCommandList");
VTableHook g_queue("ID3D12CommandQueue");

std::atomic<bool> g_installed{ false };
std::atomic<bool> g_installFailed{ false };
SRWLOCK g_installLock = SRWLOCK_INIT;
UINT g_rtvIncrement = 0;
UINT g_dsvIncrement = 0;

using PFN_CreateDevice = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
PFN_CreateDevice g_origCreateDevice = nullptr;
void* g_createDeviceTarget = nullptr;
void* g_dllNotificationCookie = nullptr;

// -------------------------------------------------------------------------------------------------
// Original-call helpers
// -------------------------------------------------------------------------------------------------
template <typename Fn, typename Self, typename... Args>
inline auto Orig(const VTableHook& hook, unsigned slot, Self* self, Args... args)
{
    return reinterpret_cast<Fn>(hook.Original(self, slot))(self, args...);
}

using CL = ID3D12GraphicsCommandList;

// -------------------------------------------------------------------------------------------------
// Command-list vtable patching
// -------------------------------------------------------------------------------------------------
// RTSky writes list slots up to Barrier (ID3D12GraphicsCommandList7). A list is only patched when it
// is a DIRECT / COMPUTE list that implements ID3D12GraphicsCommandList7 on the same interface pointer;
// anything else (video / copy lists, wrappers exposing an older interface) would have its vtable
// written past its end. Rejected vtables are remembered so the check runs once per class.
SRWLOCK g_rejectLock = SRWLOCK_INIT;
void** g_rejected[32] = {};
uint32_t g_rejectedCount = 0;

bool IsRejectedVtable(void** vtable)
{
    AcquireSRWLockShared(&g_rejectLock);
    bool found = false;
    for (uint32_t i = 0; i < g_rejectedCount && !found; ++i)
        found = g_rejected[i] == vtable;
    ReleaseSRWLockShared(&g_rejectLock);
    return found;
}

void RejectVtable(void** vtable, D3D12_COMMAND_LIST_TYPE type)
{
    AcquireSRWLockExclusive(&g_rejectLock);
    if (g_rejectedCount < 32)
        g_rejected[g_rejectedCount++] = vtable;
    ReleaseSRWLockExclusive(&g_rejectLock);
    if (type == D3D12_COMMAND_LIST_TYPE_DIRECT || type == D3D12_COMMAND_LIST_TYPE_COMPUTE)
        LOG_WARN("Command list vtable %p (type %d) does not expose ID3D12GraphicsCommandList7; not tracked",
                 static_cast<void*>(vtable), static_cast<int>(type));
}

// Returns true if the list is (now) patched.
bool PatchList(ID3D12CommandList* list)
{
    if (list == nullptr || !g_installed.load())
        return false;
    if (g_list.IsKnownFast(list))
        return true;
    void** vtable = *reinterpret_cast<void***>(list);
    if (IsRejectedVtable(vtable))
        return false;
    const D3D12_COMMAND_LIST_TYPE type = list->GetType();
    bool ok = type == D3D12_COMMAND_LIST_TYPE_DIRECT || type == D3D12_COMMAND_LIST_TYPE_COMPUTE;
    if (ok)
    {
        ComPtr<ID3D12GraphicsCommandList7> list7;
        ok = SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&list7))) &&
             static_cast<void*>(list7.Get()) == static_cast<void*>(list);
    }
    if (!ok)
    {
        RejectVtable(vtable, type);
        return false;
    }
    return g_list.Patch(list);
}

// -------------------------------------------------------------------------------------------------
// Device hooks
// -------------------------------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE Device_CreateCommandList(ID3D12Device* self, UINT nodeMask, D3D12_COMMAND_LIST_TYPE type,
                                                   ID3D12CommandAllocator* allocator, ID3D12PipelineState* initial, REFIID riid,
                                                   void** list)
{
    using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*,
                                           ID3D12PipelineState*, REFIID, void**);
    HRESULT hr = Orig<Fn>(g_device, RTSKY_IDX_Device_CreateCommandList, self, nodeMask, type, allocator, initial, riid, list);
    if (SUCCEEDED(hr) && list != nullptr && *list != nullptr && !HookBypass::Active() &&
        (type == D3D12_COMMAND_LIST_TYPE_DIRECT || type == D3D12_COMMAND_LIST_TYPE_COMPUTE))
    {
        ComPtr<CL> cl;
        if (SUCCEEDED(static_cast<IUnknown*>(*list)->QueryInterface(IID_PPV_ARGS(&cl))) && PatchList(cl.Get()))
        {
            // A new list is created open: start its state like a Reset would.
            track::OnReset(*track::GetListState(cl.Get()), initial);
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Device_CreateCommandList1(ID3D12Device* self, UINT nodeMask, D3D12_COMMAND_LIST_TYPE type,
                                                    D3D12_COMMAND_LIST_FLAGS flags, REFIID riid, void** list)
{
    using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE, D3D12_COMMAND_LIST_FLAGS, REFIID, void**);
    HRESULT hr = Orig<Fn>(g_device, RTSKY_IDX_Device_CreateCommandList1, self, nodeMask, type, flags, riid, list);
    if (SUCCEEDED(hr) && list != nullptr && *list != nullptr && !HookBypass::Active() &&
        (type == D3D12_COMMAND_LIST_TYPE_DIRECT || type == D3D12_COMMAND_LIST_TYPE_COMPUTE))
    {
        ComPtr<CL> cl;
        if (SUCCEEDED(static_cast<IUnknown*>(*list)->QueryInterface(IID_PPV_ARGS(&cl))))
            PatchList(cl.Get()); // created closed; state starts at its first Reset
    }
    return hr;
}

void STDMETHODCALLTYPE Device_CreateShaderResourceView(ID3D12Device* self, ID3D12Resource* resource,
                                                       const D3D12_SHADER_RESOURCE_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    if (!HookBypass::Active())
        track::Descriptors().OnCreateSRV(resource, desc, handle);
    using Fn = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
    Orig<Fn>(g_device, RTSKY_IDX_Device_CreateShaderResourceView, self, resource, desc, handle);
}

void STDMETHODCALLTYPE Device_CreateRenderTargetView(ID3D12Device* self, ID3D12Resource* resource,
                                                     const D3D12_RENDER_TARGET_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    if (!HookBypass::Active())
        track::Descriptors().OnCreateRTV(resource, desc, handle);
    using Fn = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
    Orig<Fn>(g_device, RTSKY_IDX_Device_CreateRenderTargetView, self, resource, desc, handle);
}

void STDMETHODCALLTYPE Device_CreateDepthStencilView(ID3D12Device* self, ID3D12Resource* resource,
                                                     const D3D12_DEPTH_STENCIL_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    if (!HookBypass::Active())
        track::Descriptors().OnCreateDSV(resource, desc, handle);
    using Fn = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_DEPTH_STENCIL_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
    Orig<Fn>(g_device, RTSKY_IDX_Device_CreateDepthStencilView, self, resource, desc, handle);
}

// Copies of RTV / DSV descriptors between heaps carry the tracked view information along.
void CopyTracked(D3D12_CPU_DESCRIPTOR_HANDLE dst, D3D12_CPU_DESCRIPTOR_HANDLE src, UINT count, UINT increment)
{
    for (UINT i = 0; i < count; ++i)
        track::Descriptors().Copy({ dst.ptr + SIZE_T(i) * increment }, { src.ptr + SIZE_T(i) * increment });
}

UINT IncrementFor(D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    return type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV ? g_rtvIncrement : type == D3D12_DESCRIPTOR_HEAP_TYPE_DSV ? g_dsvIncrement : 0;
}

void STDMETHODCALLTYPE Device_CopyDescriptorsSimple(ID3D12Device* self, UINT count, D3D12_CPU_DESCRIPTOR_HANDLE dst,
                                                    D3D12_CPU_DESCRIPTOR_HANDLE src, D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    using Fn = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE);
    Orig<Fn>(g_device, RTSKY_IDX_Device_CopyDescriptorsSimple, self, count, dst, src, type);
    const UINT inc = IncrementFor(type);
    if (inc != 0 && !HookBypass::Active())
        CopyTracked(dst, src, count, inc);
}

void STDMETHODCALLTYPE Device_CopyDescriptors(ID3D12Device* self, UINT numDst, const D3D12_CPU_DESCRIPTOR_HANDLE* dstStarts,
                                              const UINT* dstSizes, UINT numSrc, const D3D12_CPU_DESCRIPTOR_HANDLE* srcStarts,
                                              const UINT* srcSizes, D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    using Fn = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, UINT,
                                        const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, D3D12_DESCRIPTOR_HEAP_TYPE);
    Orig<Fn>(g_device, RTSKY_IDX_Device_CopyDescriptors, self, numDst, dstStarts, dstSizes, numSrc, srcStarts, srcSizes, type);
    const UINT inc = IncrementFor(type);
    if (inc == 0 || HookBypass::Active() || dstStarts == nullptr || srcStarts == nullptr)
        return;
    // Walk both range lists in lock step (sizes default to 1 when the size array is null).
    UINT di = 0, si = 0, dOff = 0, sOff = 0;
    while (di < numDst && si < numSrc)
    {
        const UINT dSize = dstSizes != nullptr ? dstSizes[di] : 1;
        const UINT sSize = srcSizes != nullptr ? srcSizes[si] : 1;
        const UINT n = std::min(dSize - dOff, sSize - sOff);
        CopyTracked({ dstStarts[di].ptr + SIZE_T(dOff) * inc }, { srcStarts[si].ptr + SIZE_T(sOff) * inc }, n, inc);
        dOff += n;
        sOff += n;
        if (dOff >= dSize) { ++di; dOff = 0; }
        if (sOff >= sSize) { ++si; sOff = 0; }
    }
}

// -------------------------------------------------------------------------------------------------
// Command list hooks
// -------------------------------------------------------------------------------------------------
inline ListState* Track(CL* self)
{
    return HookBypass::Active() ? nullptr : track::GetListState(self);
}

// Closes the current binding (if any) and gives the renderer the chance to inject.
void CloseAndInject(CL* self, ListState& s)
{
    if (s.inRenderPass)
        return; // nothing may be recorded inside a render pass
    if (const track::BindingRecord* r = track::CloseBinding(s))
        render::OnBindingClosed(self, s, *r);
}

HRESULT STDMETHODCALLTYPE CL_Close(CL* self)
{
    if (ListState* s = Track(self))
        CloseAndInject(self, *s);
    using Fn = HRESULT(STDMETHODCALLTYPE*)(CL*);
    return Orig<Fn>(g_list, RTSKY_IDX_CL_Close, self);
}

HRESULT STDMETHODCALLTYPE CL_Reset(CL* self, ID3D12CommandAllocator* allocator, ID3D12PipelineState* initial)
{
    using Fn = HRESULT(STDMETHODCALLTYPE*)(CL*, ID3D12CommandAllocator*, ID3D12PipelineState*);
    HRESULT hr = Orig<Fn>(g_list, RTSKY_IDX_CL_Reset, self, allocator, initial);
    if (SUCCEEDED(hr))
    {
        if (ListState* s = Track(self))
            track::OnReset(*s, initial);
    }
    return hr;
}

void STDMETHODCALLTYPE CL_DrawInstanced(CL* self, UINT a, UINT b, UINT c, UINT d)
{
    if (ListState* s = Track(self))
        track::OnDraw(*s);
    using Fn = void(STDMETHODCALLTYPE*)(CL*, UINT, UINT, UINT, UINT);
    Orig<Fn>(g_list, RTSKY_IDX_CL_DrawInstanced, self, a, b, c, d);
}

void STDMETHODCALLTYPE CL_DrawIndexedInstanced(CL* self, UINT a, UINT b, UINT c, INT d, UINT e)
{
    if (ListState* s = Track(self))
        track::OnDraw(*s);
    using Fn = void(STDMETHODCALLTYPE*)(CL*, UINT, UINT, UINT, INT, UINT);
    Orig<Fn>(g_list, RTSKY_IDX_CL_DrawIndexedInstanced, self, a, b, c, d, e);
}

void STDMETHODCALLTYPE CL_ExecuteIndirect(CL* self, ID3D12CommandSignature* sig, UINT maxCount, ID3D12Resource* args, UINT64 argsOffset,
                                          ID3D12Resource* countBuffer, UINT64 countOffset)
{
    if (ListState* s = Track(self))
        track::OnDraw(*s);
    using Fn = void(STDMETHODCALLTYPE*)(CL*, ID3D12CommandSignature*, UINT, ID3D12Resource*, UINT64, ID3D12Resource*, UINT64);
    Orig<Fn>(g_list, RTSKY_IDX_CL_ExecuteIndirect, self, sig, maxCount, args, argsOffset, countBuffer, countOffset);
}

void STDMETHODCALLTYPE CL_RSSetViewports(CL* self, UINT count, const D3D12_VIEWPORT* viewports)
{
    if (ListState* s = Track(self))
        track::OnViewports(*s, count, viewports);
    using Fn = void(STDMETHODCALLTYPE*)(CL*, UINT, const D3D12_VIEWPORT*);
    Orig<Fn>(g_list, RTSKY_IDX_CL_RSSetViewports, self, count, viewports);
}

void STDMETHODCALLTYPE CL_SetPipelineState(CL* self, ID3D12PipelineState* pso)
{
    if (ListState* s = Track(self))
    {
        s->pipelineKind = ListState::PipelineKind::Pso;
        s->pso = pso;
        s->stateObject = nullptr;
    }
    using Fn = void(STDMETHODCALLTYPE*)(CL*, ID3D12PipelineState*);
    Orig<Fn>(g_list, RTSKY_IDX_CL_SetPipelineState, self, pso);
}

void STDMETHODCALLTYPE CL_SetPipelineState1(CL* self, ID3D12StateObject* stateObject)
{
    if (ListState* s = Track(self))
    {
        s->pipelineKind = ListState::PipelineKind::StateObject;
        s->stateObject = stateObject;
        s->pso = nullptr;
    }
    using Fn = void(STDMETHODCALLTYPE*)(CL*, ID3D12StateObject*);
    Orig<Fn>(g_list, RTSKY_IDX_CL_SetPipelineState1, self, stateObject);
}

void STDMETHODCALLTYPE CL_ResourceBarrier(CL* self, UINT count, const D3D12_RESOURCE_BARRIER* barriers)
{
    if (ListState* s = Track(self))
        track::OnResourceBarrier(*s, count, barriers);
    using Fn = void(STDMETHODCALLTYPE*)(CL*, UINT, const D3D12_RESOURCE_BARRIER*);
    Orig<Fn>(g_list, RTSKY_IDX_CL_ResourceBarrier, self, count, barriers);
}

void STDMETHODCALLTYPE CL_Barrier(CL* self, UINT32 count, const D3D12_BARRIER_GROUP* groups)
{
    if (ListState* s = Track(self))
        track::OnEnhancedBarrier(*s, count, groups);
    using Fn = void(STDMETHODCALLTYPE*)(CL*, UINT32, const D3D12_BARRIER_GROUP*);
    Orig<Fn>(g_list, RTSKY_IDX_CL_Barrier, self, count, groups);
}

void STDMETHODCALLTYPE CL_SetDescriptorHeaps(CL* self, UINT count, ID3D12DescriptorHeap* const* heaps)
{
    if (ListState* s = Track(self))
    {
        const UINT n = count > 2 ? 2 : count;
        bool changed = n != s->heapCount;
        for (UINT i = 0; i < n; ++i)
            changed |= s->heaps[i] != heaps[i];
        for (UINT i = 0; i < 2; ++i)
            s->heaps[i] = i < n ? heaps[i] : nullptr;
        s->heapCount = n;
        if (changed)
        {
            // Descriptor tables are undefined after a heap change (D3D12 spec): forget them.
            for (auto& a : s->compute.args)
                if (a.type == track::BindPointState::ArgTable)
                    a = {};
            for (auto& a : s->graphics.args)
                if (a.type == track::BindPointState::ArgTable)
                    a = {};
        }
    }
    using Fn = void(STDMETHODCALLTYPE*)(CL*, UINT, ID3D12DescriptorHeap* const*);
    Orig<Fn>(g_list, RTSKY_IDX_CL_SetDescriptorHeaps, self, count, heaps);
}

void SetRootSignature(track::BindPointState& bp, ID3D12RootSignature* rs)
{
    if (bp.rootSignature != rs)
        bp.ClearArgs();
    bp.rootSignature = rs;
}

void STDMETHODCALLTYPE CL_SetComputeRootSignature(CL* self, ID3D12RootSignature* rs)
{
    if (ListState* s = Track(self))
        SetRootSignature(s->compute, rs);
    using Fn = void(STDMETHODCALLTYPE*)(CL*, ID3D12RootSignature*);
    Orig<Fn>(g_list, RTSKY_IDX_CL_SetComputeRootSignature, self, rs);
}

void STDMETHODCALLTYPE CL_SetGraphicsRootSignature(CL* self, ID3D12RootSignature* rs)
{
    if (ListState* s = Track(self))
        SetRootSignature(s->graphics, rs);
    using Fn = void(STDMETHODCALLTYPE*)(CL*, ID3D12RootSignature*);
    Orig<Fn>(g_list, RTSKY_IDX_CL_SetGraphicsRootSignature, self, rs);
}

void SetArg(track::BindPointState& bp, UINT index, track::BindPointState::ArgType type, uint64_t value)
{
    if (index < track::BindPointState::kMaxRootParams)
        bp.args[index] = { type, value };
}

#define RTSKY_ROOT_ARG_HOOK(Name, Point, Type, ValueType, ToValue)                                     \
    void STDMETHODCALLTYPE CL_##Name(CL* self, UINT index, ValueType value)                            \
    {                                                                                                  \
        if (ListState* s = Track(self))                                                                \
            SetArg(s->Point, index, track::BindPointState::Type, ToValue);                             \
        using Fn = void(STDMETHODCALLTYPE*)(CL*, UINT, ValueType);                                      \
        Orig<Fn>(g_list, RTSKY_IDX_CL_##Name, self, index, value);                                      \
    }

RTSKY_ROOT_ARG_HOOK(SetComputeRootDescriptorTable, compute, ArgTable, D3D12_GPU_DESCRIPTOR_HANDLE, value.ptr)
RTSKY_ROOT_ARG_HOOK(SetGraphicsRootDescriptorTable, graphics, ArgTable, D3D12_GPU_DESCRIPTOR_HANDLE, value.ptr)
RTSKY_ROOT_ARG_HOOK(SetComputeRootConstantBufferView, compute, ArgCBV, D3D12_GPU_VIRTUAL_ADDRESS, value)
RTSKY_ROOT_ARG_HOOK(SetGraphicsRootConstantBufferView, graphics, ArgCBV, D3D12_GPU_VIRTUAL_ADDRESS, value)
RTSKY_ROOT_ARG_HOOK(SetComputeRootShaderResourceView, compute, ArgSRV, D3D12_GPU_VIRTUAL_ADDRESS, value)
RTSKY_ROOT_ARG_HOOK(SetGraphicsRootShaderResourceView, graphics, ArgSRV, D3D12_GPU_VIRTUAL_ADDRESS, value)
RTSKY_ROOT_ARG_HOOK(SetComputeRootUnorderedAccessView, compute, ArgUAV, D3D12_GPU_VIRTUAL_ADDRESS, value)
RTSKY_ROOT_ARG_HOOK(SetGraphicsRootUnorderedAccessView, graphics, ArgUAV, D3D12_GPU_VIRTUAL_ADDRESS, value)
#undef RTSKY_ROOT_ARG_HOOK

void STDMETHODCALLTYPE CL_SetComputeRoot32BitConstant(CL* self, UINT index, UINT value, UINT offset)
{
    if (ListState* s = Track(self))
        s->compute.SetConstant(index, offset, value);
    using Fn = void(STDMETHODCALLTYPE*)(CL*, UINT, UINT, UINT);
    Orig<Fn>(g_list, RTSKY_IDX_CL_SetComputeRoot32BitConstant, self, index, value, offset);
}

void STDMETHODCALLTYPE CL_SetGraphicsRoot32BitConstant(CL* self, UINT index, UINT value, UINT offset)
{
    if (ListState* s = Track(self))
        s->graphics.SetConstant(index, offset, value);
    using Fn = void(STDMETHODCALLTYPE*)(CL*, UINT, UINT, UINT);
    Orig<Fn>(g_list, RTSKY_IDX_CL_SetGraphicsRoot32BitConstant, self, index, value, offset);
}

void STDMETHODCALLTYPE CL_SetComputeRoot32BitConstants(CL* self, UINT index, UINT count, const void* data, UINT offset)
{
    if (ListState* s = Track(self))
    {
        const uint32_t* v = static_cast<const uint32_t*>(data);
        for (UINT i = 0; i < count && v != nullptr; ++i)
            s->compute.SetConstant(index, offset + i, v[i]);
    }
    using Fn = void(STDMETHODCALLTYPE*)(CL*, UINT, UINT, const void*, UINT);
    Orig<Fn>(g_list, RTSKY_IDX_CL_SetComputeRoot32BitConstants, self, index, count, data, offset);
}

void STDMETHODCALLTYPE CL_SetGraphicsRoot32BitConstants(CL* self, UINT index, UINT count, const void* data, UINT offset)
{
    if (ListState* s = Track(self))
    {
        const uint32_t* v = static_cast<const uint32_t*>(data);
        for (UINT i = 0; i < count && v != nullptr; ++i)
            s->graphics.SetConstant(index, offset + i, v[i]);
    }
    using Fn = void(STDMETHODCALLTYPE*)(CL*, UINT, UINT, const void*, UINT);
    Orig<Fn>(g_list, RTSKY_IDX_CL_SetGraphicsRoot32BitConstants, self, index, count, data, offset);
}

void STDMETHODCALLTYPE CL_OMSetRenderTargets(CL* self, UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, BOOL singleRange,
                                             const D3D12_CPU_DESCRIPTOR_HANDLE* dsv)
{
    if (ListState* s = Track(self))
    {
        if (!s->inRenderPass)
        {
            CloseAndInject(self, *s);
            track::OnSetRenderTargets(*s, count, rtvs, singleRange, dsv, g_rtvIncrement);
        }
    }
    using Fn = void(STDMETHODCALLTYPE*)(CL*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);
    Orig<Fn>(g_list, RTSKY_IDX_CL_OMSetRenderTargets, self, count, rtvs, singleRange, dsv);
}

void STDMETHODCALLTYPE CL_ClearDepthStencilView(CL* self, D3D12_CPU_DESCRIPTOR_HANDLE dsv, D3D12_CLEAR_FLAGS flags, FLOAT depth,
                                                UINT8 stencil, UINT numRects, const D3D12_RECT* rects)
{
    if (ListState* s = Track(self))
        track::OnClearDepth(*s, dsv, flags, depth);
    using Fn = void(STDMETHODCALLTYPE*)(CL*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT, const D3D12_RECT*);
    Orig<Fn>(g_list, RTSKY_IDX_CL_ClearDepthStencilView, self, dsv, flags, depth, stencil, numRects, rects);
}

void STDMETHODCALLTYPE CL_BeginRenderPass(CL* self, UINT count, const D3D12_RENDER_PASS_RENDER_TARGET_DESC* rts,
                                          const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* ds, D3D12_RENDER_PASS_FLAGS flags)
{
    if (ListState* s = Track(self))
    {
        CloseAndInject(self, *s);
        track::OnBeginRenderPass(*s, count, rts, ds, flags);
    }
    using Fn = void(STDMETHODCALLTYPE*)(CL*, UINT, const D3D12_RENDER_PASS_RENDER_TARGET_DESC*,
                                        const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC*, D3D12_RENDER_PASS_FLAGS);
    Orig<Fn>(g_list, RTSKY_IDX_CL_BeginRenderPass, self, count, rts, ds, flags);
}

void STDMETHODCALLTYPE CL_EndRenderPass(CL* self)
{
    using Fn = void(STDMETHODCALLTYPE*)(CL*);
    Orig<Fn>(g_list, RTSKY_IDX_CL_EndRenderPass, self);
    if (ListState* s = Track(self))
    {
        s->inRenderPass = false;
        CloseAndInject(self, *s); // after the pass has ended: recording is legal again
    }
}

void STDMETHODCALLTYPE CL_BuildRaytracingAccelerationStructure(CL* self, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* desc,
                                                               UINT numPostbuild,
                                                               const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* postbuild)
{
    using Fn = void(STDMETHODCALLTYPE*)(CL*, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC*, UINT,
                                        const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC*);
    Orig<Fn>(g_list, RTSKY_IDX_CL_BuildRaytracingAccelerationStructure, self, desc, numPostbuild, postbuild);
    if (ListState* s = Track(self))
    {
        ComPtr<ID3D12GraphicsCommandList4> list4;
        if (SUCCEEDED(self->QueryInterface(IID_PPV_ARGS(&list4))))
        {
            HookBypass bypass;
            track::Tlas().OnBuild(list4.Get(), *s, desc);
        }
    }
}

void STDMETHODCALLTYPE CL_DispatchRays(CL* self, const D3D12_DISPATCH_RAYS_DESC* desc)
{
    if (ListState* s = Track(self))
        ++s->dispatchRays;
    using Fn = void(STDMETHODCALLTYPE*)(CL*, const D3D12_DISPATCH_RAYS_DESC*);
    Orig<Fn>(g_list, RTSKY_IDX_CL_DispatchRays, self, desc);
}

// -------------------------------------------------------------------------------------------------
// Queue hook
// -------------------------------------------------------------------------------------------------
void STDMETHODCALLTYPE Queue_ExecuteCommandLists(ID3D12CommandQueue* self, UINT count, ID3D12CommandList* const* lists)
{
    const bool track = !HookBypass::Active() && count > 0 && lists != nullptr;
    if (track)
    {
        // Lists whose vtable we have not seen (debug layer, runtime bypass, other classes) are
        // patched now; they are tracked from their next Reset on (see ListState::sawReset).
        for (UINT i = 0; i < count; ++i)
            PatchList(lists[i]);
        track::Analyzer().OnExecute(self, count, lists);

        for (UINT i = 0; i < count; ++i)
        {
            const ListState* s = track::FindListState(lists[i]);
            if (s == nullptr)
                continue;
            if (s->injectedComposite)
                track::Tlas().NoteConsumerQueue(self);
            if (!s->tlasConsumedValid)
                continue;
            // The trace reads a clone whose producer was submitted on another queue and has not
            // completed. Only possible when the composite moved to a different queue since the clone
            // was chosen (normally a clone is either from this queue or already complete): order this
            // submission after the producer's (already signalled) fence.
            const track::TlasInfo& t = s->tlasConsumed;
            if (t.producerQueue != nullptr && t.producerQueue != self && !track::TlasTracker::ProducerDone(t))
            {
                RTSKY_LOG_ONCE(log::Level::Warning, "Composite list moved to another queue; inserting a queue wait for the TLAS clone");
                HookBypass bypass;
                self->Wait(t.producerFence.Get(), t.producerFenceValue);
            }
        }
    }
    using Fn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
    Orig<Fn>(g_queue, RTSKY_IDX_Queue_ExecuteCommandLists, self, count, lists);
    if (track)
    {
        ComPtr<ID3D12Fence> fence;
        uint64_t value = 0;
        const bool signalled = render::Lifetime().OnExecuted(self, count, lists, &fence, &value);
        // Scene TLAS clones recorded in these lists become visible to other lists only now.
        for (UINT i = 0; signalled && i < count; ++i)
        {
            const ListState* s = track::FindListState(lists[i]);
            if (s != nullptr && s->tlasProducedValid)
                track::Tlas().Publish(s->tlasProduced, self, fence.Get(), value);
        }
        render::Lifetime().Collect();
    }
}

// -------------------------------------------------------------------------------------------------
// Installation
// -------------------------------------------------------------------------------------------------
void RegisterHooks()
{
    g_device.AddHook(RTSKY_IDX_Device_CreateCommandList, reinterpret_cast<void*>(&Device_CreateCommandList));
    g_device.AddHook(RTSKY_IDX_Device_CreateCommandList1, reinterpret_cast<void*>(&Device_CreateCommandList1));
    g_device.AddHook(RTSKY_IDX_Device_CreateShaderResourceView, reinterpret_cast<void*>(&Device_CreateShaderResourceView));
    g_device.AddHook(RTSKY_IDX_Device_CreateRenderTargetView, reinterpret_cast<void*>(&Device_CreateRenderTargetView));
    g_device.AddHook(RTSKY_IDX_Device_CreateDepthStencilView, reinterpret_cast<void*>(&Device_CreateDepthStencilView));
    g_device.AddHook(RTSKY_IDX_Device_CopyDescriptors, reinterpret_cast<void*>(&Device_CopyDescriptors));
    g_device.AddHook(RTSKY_IDX_Device_CopyDescriptorsSimple, reinterpret_cast<void*>(&Device_CopyDescriptorsSimple));

#define RTSKY_HOOK(Name) g_list.AddHook(RTSKY_IDX_CL_##Name, reinterpret_cast<void*>(&CL_##Name))
    RTSKY_HOOK(Close);
    RTSKY_HOOK(Reset);
    RTSKY_HOOK(DrawInstanced);
    RTSKY_HOOK(DrawIndexedInstanced);
    RTSKY_HOOK(ExecuteIndirect);
    RTSKY_HOOK(RSSetViewports);
    RTSKY_HOOK(SetPipelineState);
    RTSKY_HOOK(SetPipelineState1);
    RTSKY_HOOK(ResourceBarrier);
    RTSKY_HOOK(Barrier);
    RTSKY_HOOK(SetDescriptorHeaps);
    RTSKY_HOOK(SetComputeRootSignature);
    RTSKY_HOOK(SetGraphicsRootSignature);
    RTSKY_HOOK(SetComputeRootDescriptorTable);
    RTSKY_HOOK(SetGraphicsRootDescriptorTable);
    RTSKY_HOOK(SetComputeRoot32BitConstant);
    RTSKY_HOOK(SetGraphicsRoot32BitConstant);
    RTSKY_HOOK(SetComputeRoot32BitConstants);
    RTSKY_HOOK(SetGraphicsRoot32BitConstants);
    RTSKY_HOOK(SetComputeRootConstantBufferView);
    RTSKY_HOOK(SetGraphicsRootConstantBufferView);
    RTSKY_HOOK(SetComputeRootShaderResourceView);
    RTSKY_HOOK(SetGraphicsRootShaderResourceView);
    RTSKY_HOOK(SetComputeRootUnorderedAccessView);
    RTSKY_HOOK(SetGraphicsRootUnorderedAccessView);
    RTSKY_HOOK(OMSetRenderTargets);
    RTSKY_HOOK(ClearDepthStencilView);
    RTSKY_HOOK(BeginRenderPass);
    RTSKY_HOOK(EndRenderPass);
    RTSKY_HOOK(BuildRaytracingAccelerationStructure);
    RTSKY_HOOK(DispatchRays);
#undef RTSKY_HOOK

    g_queue.AddHook(RTSKY_IDX_Queue_ExecuteCommandLists, reinterpret_cast<void*>(&Queue_ExecuteCommandLists));
}

// Patches the device, queue and command list vtables using objects created on `device`.
bool PatchFromDevice(ID3D12Device* device)
{
    AcquireSRWLockExclusive(&g_installLock);
    if (g_installFailed.load())
    {
        ReleaseSRWLockExclusive(&g_installLock);
        return false;
    }
    if (g_installed.load())
    {
        // Another device (e.g. created by an overlay) - its classes are the same; just make sure.
        g_device.Patch(device);
        ReleaseSRWLockExclusive(&g_installLock);
        return true;
    }

    static bool registered = false;
    if (!registered)
    {
        RegisterHooks();
        registered = true;
    }

    HookBypass bypass;
    g_rtvIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    g_dsvIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    bool ok = g_device.Patch(device);

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> directAlloc, computeAlloc;
    ComPtr<ID3D12GraphicsCommandList> directList, computeList;
    ok &= SUCCEEDED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
    ok &= SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&directAlloc)));
    ok &= SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&computeAlloc)));
    if (ok)
    {
        ok &= SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, directAlloc.Get(), nullptr, IID_PPV_ARGS(&directList)));
        ok &= SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, computeAlloc.Get(), nullptr, IID_PPV_ARGS(&computeList)));
    }
    if (ok)
    {
        // Make sure the list really exposes the DXR / enhanced-barrier slots we patch.
        ComPtr<ID3D12GraphicsCommandList7> list7;
        if (FAILED(directList.As(&list7)))
        {
            LOG_ERROR("ID3D12GraphicsCommandList7 is not available (outdated D3D12 runtime) - RTSky disabled");
            ok = false;
        }
    }
    if (ok)
    {
        ok &= g_queue.Patch(queue.Get());
        ok &= g_list.Patch(directList.Get());
        ok &= g_list.Patch(computeList.Get());
    }
    if (directList)
        directList->Close();
    if (computeList)
        computeList->Close();

    if (ok)
    {
        g_installed.store(true);
    }
    else
    {
        // Leave nothing half-installed: hooks without a complete set would track lists partially.
        g_list.Unpatch();
        g_queue.Unpatch();
        g_device.Unpatch();
        g_installFailed.store(true);
    }
    ReleaseSRWLockExclusive(&g_installLock);
    if (ok)
        LOG_INFO("D3D12 hooks installed (device %p)", static_cast<void*>(device));
    else
        LOG_ERROR("D3D12 hook installation failed");
    return ok;
}

HRESULT WINAPI Detour_D3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL level, REFIID riid, void** device)
{
    HRESULT hr = g_origCreateDevice(adapter, level, riid, device);
    if (SUCCEEDED(hr) && device != nullptr && *device != nullptr && !HookBypass::Active())
    {
        ComPtr<ID3D12Device> d;
        if (SUCCEEDED(static_cast<IUnknown*>(*device)->QueryInterface(IID_PPV_ARGS(&d))))
        {
            LOG_INFO("Game created its D3D12 device (feature level 0x%x)", static_cast<unsigned>(level));
            PatchFromDevice(d.Get());
        }
    }
    return hr;
}

bool HookCreateDevice(HMODULE d3d12)
{
    if (g_createDeviceTarget != nullptr)
        return true;
    void* target = reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12CreateDevice"));
    if (target == nullptr)
        return false;
    if (MH_CreateHook(target, reinterpret_cast<void*>(&Detour_D3D12CreateDevice), reinterpret_cast<void**>(&g_origCreateDevice)) != MH_OK ||
        MH_EnableHook(target) != MH_OK)
    {
        LOG_ERROR("Could not detour D3D12CreateDevice");
        return false;
    }
    g_createDeviceTarget = target;
    LOG_INFO("D3D12CreateDevice detoured");
    return true;
}

// ntdll loader notifications (not in the public SDK headers)
struct LdrDllLoadedNotificationData
{
    ULONG Flags;
    const UNICODE_STRING* FullDllName;
    const UNICODE_STRING* BaseDllName;
    void* DllBase;
    ULONG SizeOfImage;
};
union LdrDllNotificationData
{
    LdrDllLoadedNotificationData Loaded;
};
using LdrDllNotificationFn = VOID(CALLBACK*)(ULONG reason, const LdrDllNotificationData* data, void* context);
using LdrRegisterDllNotificationFn = LONG(NTAPI*)(ULONG flags, LdrDllNotificationFn callback, void* context, void** cookie);
using LdrUnregisterDllNotificationFn = LONG(NTAPI*)(void* cookie);
constexpr ULONG kLdrReasonLoaded = 1;

VOID CALLBACK OnDllNotification(ULONG reason, const LdrDllNotificationData* data, void*)
{
    if (reason != kLdrReasonLoaded || data == nullptr || data->Loaded.BaseDllName == nullptr)
        return;
    const UNICODE_STRING* name = data->Loaded.BaseDllName;
    const wchar_t kName[] = L"d3d12.dll";
    const size_t len = name->Length / sizeof(wchar_t);
    if (len != wcslen(kName) || _wcsnicmp(name->Buffer, kName, len) != 0)
        return;
    HookCreateDevice(static_cast<HMODULE>(data->Loaded.DllBase));
}

} // namespace

bool InstallEarly()
{
    if (MH_Initialize() != MH_OK)
    {
        LOG_ERROR("MinHook initialisation failed");
        return false;
    }
    if (HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll"))
    {
        LOG_INFO("d3d12.dll was already loaded when RTSky started");
        return HookCreateDevice(d3d12);
    }
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto reg = reinterpret_cast<LdrRegisterDllNotificationFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "LdrRegisterDllNotification")));
    if (reg == nullptr || reg(0, &OnDllNotification, nullptr, &g_dllNotificationCookie) != 0)
    {
        LOG_ERROR("LdrRegisterDllNotification failed");
        return false;
    }
    LOG_INFO("Waiting for d3d12.dll to load");
    return true;
}

bool InstallLate()
{
    if (g_installed.load())
        return true;
    static bool attempted = false;
    if (attempted)
        return false;
    attempted = true;

    LOG_WARN("The game's D3D12 device was created before RTSky loaded; using the late hook path. Render targets that already "
             "exist are unknown until the game recreates them - change the resolution or window mode once if RTSky reports that "
             "no G-buffer was found.");

    HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    if (dxgi == nullptr || d3d12 == nullptr)
        return false;
    using CreateFactoryFn = HRESULT(WINAPI*)(UINT, REFIID, void**);
    auto createFactory = reinterpret_cast<CreateFactoryFn>(reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory2")));
    PFN_CreateDevice createDevice = g_origCreateDevice != nullptr
        ? g_origCreateDevice
        : reinterpret_cast<PFN_CreateDevice>(reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12CreateDevice")));
    if (createFactory == nullptr || createDevice == nullptr)
        return false;

    HookBypass bypass;
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(createFactory(0, IID_PPV_ARGS(&factory))))
        return false;
    // D3D12 devices are singletons per adapter: on the game's adapter this returns the game's device.
    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter))))
        return false;
    ComPtr<ID3D12Device> device;
    if (FAILED(createDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))))
        return false;
    return PatchFromDevice(device.Get());
}

bool Installed()
{
    return g_installed.load();
}

void VerifyHooks()
{
    if (!g_installed.load())
        return;
    g_device.Verify();
    g_list.Verify();
    g_queue.Verify();
}

void Uninstall()
{
    g_list.Unpatch();
    g_queue.Unpatch();
    g_device.Unpatch();
    if (g_createDeviceTarget != nullptr)
        MH_DisableHook(g_createDeviceTarget);
    MH_Uninitialize();
}

} // namespace rtsky::hooks
