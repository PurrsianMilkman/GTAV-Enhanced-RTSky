// RTSky - per command list recorded state
#include "CommandListTracker.h"
#include "DescriptorTracker.h"

#include <windows.h>

#include <atomic>
#include <memory>

namespace rtsky::track {
namespace {

// list pointer -> state. States are never freed: a new list at a recycled address reuses the
// object, which keeps the thread_local cache below valid.
SRWLOCK g_listLock = SRWLOCK_INIT;
std::unordered_map<ID3D12CommandList*, std::unique_ptr<ListState>>* g_lists = nullptr;

thread_local ID3D12GraphicsCommandList* t_cachedList = nullptr;
thread_local ListState* t_cachedState = nullptr;

std::atomic<uint64_t> g_resetSerial{ 0 };

// resource -> last barrier API used by the game
SRWLOCK g_apiLock = SRWLOCK_INIT;
std::unordered_map<ID3D12Resource*, BarrierApi>* g_barrierApi = nullptr;
std::atomic<uint64_t> g_enhancedBarrierCalls{ 0 };
std::atomic<uint64_t> g_legacyBarrierCalls{ 0 };

void NoteBarrierApi(ID3D12Resource* resource, BarrierApi api)
{
    if (resource == nullptr)
        return;
    AcquireSRWLockShared(&g_apiLock);
    auto it = g_barrierApi->find(resource);
    bool same = it != g_barrierApi->end() && it->second == api;
    ReleaseSRWLockShared(&g_apiLock);
    if (same)
        return;
    AcquireSRWLockExclusive(&g_apiLock);
    (*g_barrierApi)[resource] = api;
    ReleaseSRWLockExclusive(&g_apiLock);
}

BoundTarget ToBoundTarget(const ViewInfo& v)
{
    BoundTarget t;
    t.resource = v.resource;
    t.viewFormat = v.viewFormat;
    t.resourceFormat = v.resourceFormat;
    t.width = v.width;
    t.height = v.height;
    t.sampleCount = v.sampleCount;
    t.flags = v.resourceFlags;
    return t;
}

struct GlobalInit
{
    GlobalInit()
    {
        g_lists = new std::unordered_map<ID3D12CommandList*, std::unique_ptr<ListState>>();
        g_barrierApi = new std::unordered_map<ID3D12Resource*, BarrierApi>();
    }
} g_globalInit;

} // namespace

// -------------------------------------------------------------------------------------------------
// Classification
// -------------------------------------------------------------------------------------------------
bool IsFloatHdrFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return true;
    default:
        return false;
    }
}

bool IsMrtCandidate(const BindingRecord& r)
{
    return r.rtvCount >= 3 && r.hasDsv && r.draws > 0;
}

bool IsHdrCandidate(const BindingRecord& r)
{
    return r.rtvCount >= 1 && r.draws > 0 && r.rtv[0].resource != nullptr && IsFloatHdrFormat(r.rtv[0].viewFormat) &&
           r.rtv[0].sampleCount == 1;
}

// -------------------------------------------------------------------------------------------------
// BindPointState / ListState
// -------------------------------------------------------------------------------------------------
void BindPointState::ClearArgs()
{
    for (Arg& a : args)
        a = Arg{};
    constants.clear();
}

void BindPointState::SetConstant(uint32_t param, uint32_t offset, uint32_t value)
{
    if (param >= kMaxRootParams || offset > 255)
        return;
    for (ConstEntry& e : constants)
    {
        if (e.param == param && e.offset == offset)
        {
            e.value = value;
            return;
        }
    }
    constants.push_back({ static_cast<uint8_t>(param), static_cast<uint8_t>(offset), value });
}

void ListState::ResetForRecording(ID3D12PipelineState* initialPso)
{
    resetSerial = g_resetSerial.fetch_add(1, std::memory_order_relaxed) + 1;
    pipelineKind = initialPso != nullptr ? PipelineKind::Pso : PipelineKind::None;
    pso = initialPso;
    stateObject = nullptr;
    heaps[0] = heaps[1] = nullptr;
    heapCount = 0;
    compute.rootSignature = nullptr;
    compute.ClearArgs();
    graphics.rootSignature = nullptr;
    graphics.ClearArgs();
    bindingOpen = false;
    current = BindingRecord{};
    viewport = {};
    viewportValid = false;
    inRenderPass = false;
    mrtCount = 0;
    hdrCount = 0;
    bindingSeq = 0;
    log.clear();
    observed.clear();
    barrierSeq = 0;
    tlasBuilds = blasBuilds = dispatchRays = draws = 0;
    injectedPrepare = false;
    injectedComposite = false;
    attachments.clear();
}

ListState* FindListState(ID3D12CommandList* list)
{
    if (list == nullptr)
        return nullptr;
    AcquireSRWLockShared(&g_listLock);
    auto it = g_lists->find(list);
    ListState* s = it != g_lists->end() ? it->second.get() : nullptr;
    ReleaseSRWLockShared(&g_listLock);
    return s;
}

ListState* GetListState(ID3D12GraphicsCommandList* list)
{
    if (list == t_cachedList)
        return t_cachedState;

    ListState* s = FindListState(list);
    if (s == nullptr)
    {
        auto fresh = std::make_unique<ListState>();
        fresh->list = list;
        fresh->type = list->GetType();
        fresh->ResetForRecording(nullptr);
        AcquireSRWLockExclusive(&g_listLock);
        auto [it, inserted] = g_lists->emplace(list, std::move(fresh));
        s = it->second.get();
        ReleaseSRWLockExclusive(&g_listLock);
    }
    t_cachedList = list;
    t_cachedState = s;
    return s;
}

// -------------------------------------------------------------------------------------------------
// Recording
// -------------------------------------------------------------------------------------------------
void OnReset(ListState& s, ID3D12PipelineState* initialPso)
{
    s.type = s.list->GetType();
    s.ResetForRecording(initialPso);
}

const BindingRecord* CloseBinding(ListState& s)
{
    if (!s.bindingOpen)
        return nullptr;
    s.bindingOpen = false;

    BindingRecord& r = s.current;
    r.listSeq = s.bindingSeq++;
    if (IsMrtCandidate(r))
        r.mrtOrdinal = static_cast<int32_t>(s.mrtCount++);
    if (IsHdrCandidate(r))
        r.hdrOrdinal = static_cast<int32_t>(s.hdrCount++);

    // Only bindings that drew something or cleared depth matter for the analysis.
    if (r.draws == 0 && !r.dsvCleared)
        return nullptr;
    s.log.push_back(r);
    return &s.log.back();
}

static void OpenBinding(ListState& s)
{
    s.current = BindingRecord{};
    s.current.viewport = s.viewport;
    s.current.viewportValid = s.viewportValid;
    s.bindingOpen = true;
}

void OnSetRenderTargets(ListState& s, UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, BOOL singleRange,
                        const D3D12_CPU_DESCRIPTOR_HANDLE* dsv, UINT rtvDescriptorIncrement)
{
    OpenBinding(s);
    BindingRecord& r = s.current;
    count = count > D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT ? D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT : count;
    r.rtvCount = 0;
    for (UINT i = 0; i < count && rtvs != nullptr; ++i)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = singleRange ? D3D12_CPU_DESCRIPTOR_HANDLE{ rtvs[0].ptr + SIZE_T(i) * rtvDescriptorIncrement } : rtvs[i];
        ViewInfo v;
        if (Descriptors().Lookup(h, &v) && v.kind == ViewInfo::Kind::RTV)
            r.rtv[r.rtvCount] = ToBoundTarget(v);
        ++r.rtvCount;
    }
    if (dsv != nullptr)
    {
        ViewInfo v;
        if (Descriptors().Lookup(*dsv, &v) && v.kind == ViewInfo::Kind::DSV)
        {
            r.hasDsv = true;
            r.dsv = ToBoundTarget(v);
            r.dsvReadOnlyDepth = (v.dsvFlags & D3D12_DSV_FLAG_READ_ONLY_DEPTH) != 0;
            r.dsvReadOnlyStencil = (v.dsvFlags & D3D12_DSV_FLAG_READ_ONLY_STENCIL) != 0;
        }
    }
}

void OnBeginRenderPass(ListState& s, UINT count, const D3D12_RENDER_PASS_RENDER_TARGET_DESC* rts,
                       const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* ds, D3D12_RENDER_PASS_FLAGS flags)
{
    OpenBinding(s);
    s.inRenderPass = true;
    BindingRecord& r = s.current;
    r.fromRenderPass = true;
    count = count > D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT ? D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT : count;
    r.rtvCount = 0;
    for (UINT i = 0; i < count && rts != nullptr; ++i)
    {
        ViewInfo v;
        if (Descriptors().Lookup(rts[i].cpuDescriptor, &v) && v.kind == ViewInfo::Kind::RTV)
            r.rtv[r.rtvCount] = ToBoundTarget(v);
        ++r.rtvCount;
    }
    if (ds != nullptr)
    {
        ViewInfo v;
        if (Descriptors().Lookup(ds->cpuDescriptor, &v) && v.kind == ViewInfo::Kind::DSV)
        {
            r.hasDsv = true;
            r.dsv = ToBoundTarget(v);
            r.dsvReadOnlyDepth = (flags & D3D12_RENDER_PASS_FLAG_BIND_READ_ONLY_DEPTH) != 0 ||
                                 (v.dsvFlags & D3D12_DSV_FLAG_READ_ONLY_DEPTH) != 0;
            r.dsvReadOnlyStencil = (flags & D3D12_RENDER_PASS_FLAG_BIND_READ_ONLY_STENCIL) != 0 ||
                                   (v.dsvFlags & D3D12_DSV_FLAG_READ_ONLY_STENCIL) != 0;
            if (ds->DepthBeginningAccess.Type == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR)
            {
                r.dsvCleared = true;
                r.dsvClearDepth = ds->DepthBeginningAccess.Clear.ClearValue.DepthStencil.Depth;
            }
        }
    }
}

void OnDraw(ListState& s)
{
    ++s.draws;
    if (!s.bindingOpen)
        return;
    BindingRecord& r = s.current;
    if (r.draws == 0 && s.viewportValid)
    {
        r.viewport = s.viewport;
        r.viewportValid = true;
    }
    ++r.draws;
    r.barrierSeqAtLastDraw = s.barrierSeq;
}

void OnViewports(ListState& s, UINT count, const D3D12_VIEWPORT* viewports)
{
    if (count == 0 || viewports == nullptr)
        return;
    s.viewport = viewports[0];
    s.viewportValid = true;
}

void OnClearDepth(ListState& s, D3D12_CPU_DESCRIPTOR_HANDLE dsv, D3D12_CLEAR_FLAGS flags, float depth)
{
    if ((flags & D3D12_CLEAR_FLAG_DEPTH) == 0 || !s.bindingOpen)
        return;
    ViewInfo v;
    if (!Descriptors().Lookup(dsv, &v) || v.kind != ViewInfo::Kind::DSV)
        return;
    if (s.current.hasDsv && s.current.dsv.resource == v.resource)
    {
        s.current.dsvCleared = true;
        s.current.dsvClearDepth = depth;
    }
}

void OnResourceBarrier(ListState& s, UINT count, const D3D12_RESOURCE_BARRIER* barriers)
{
    g_legacyBarrierCalls.fetch_add(1, std::memory_order_relaxed);
    for (UINT i = 0; i < count; ++i)
    {
        const D3D12_RESOURCE_BARRIER& b = barriers[i];
        if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || b.Transition.pResource == nullptr)
            continue;
        if ((b.Flags & D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY) != 0)
            continue; // split barrier: the transition completes at END_ONLY
        ObservedState o;
        o.stateOrLayout = static_cast<uint32_t>(b.Transition.StateAfter);
        o.enhanced = false;
        o.seq = ++s.barrierSeq;
        o.subresource = b.Transition.Subresource;
        s.observed[b.Transition.pResource] = o;
        NoteBarrierApi(b.Transition.pResource, BarrierApi::Legacy);
    }
}

void OnEnhancedBarrier(ListState& s, UINT count, const D3D12_BARRIER_GROUP* groups)
{
    g_enhancedBarrierCalls.fetch_add(1, std::memory_order_relaxed);
    for (UINT g = 0; g < count; ++g)
    {
        if (groups[g].Type != D3D12_BARRIER_TYPE_TEXTURE)
            continue;
        for (UINT32 i = 0; i < groups[g].NumBarriers; ++i)
        {
            const D3D12_TEXTURE_BARRIER& b = groups[g].pTextureBarriers[i];
            if (b.pResource == nullptr)
                continue;
            ObservedState o;
            o.stateOrLayout = static_cast<uint32_t>(b.LayoutAfter);
            o.enhanced = true;
            o.seq = ++s.barrierSeq;
            o.subresource = (b.Subresources.NumMipLevels == 0) ? b.Subresources.IndexOrFirstMipLevel : 0xFFFFFFFFu;
            s.observed[b.pResource] = o;
            NoteBarrierApi(b.pResource, BarrierApi::Enhanced);
        }
    }
}

bool FindObservedState(const ListState& s, ID3D12Resource* resource, uint32_t afterSeq, ObservedState* out)
{
    auto it = s.observed.find(resource);
    if (it == s.observed.end() || it->second.seq <= afterSeq)
        return false;
    *out = it->second;
    return true;
}

BarrierApi LastBarrierApi(ID3D12Resource* resource)
{
    AcquireSRWLockShared(&g_apiLock);
    auto it = g_barrierApi->find(resource);
    BarrierApi api = it != g_barrierApi->end() ? it->second : BarrierApi::Unknown;
    ReleaseSRWLockShared(&g_apiLock);
    return api;
}

bool GameUsesEnhancedBarriers()
{
    return g_enhancedBarrierCalls.load(std::memory_order_relaxed) > 0 &&
           g_legacyBarrierCalls.load(std::memory_order_relaxed) == 0;
}

// -------------------------------------------------------------------------------------------------
// Restore
// -------------------------------------------------------------------------------------------------
static void RestoreBindPoint(ID3D12GraphicsCommandList* list, const BindPointState& bp, bool compute)
{
    if (bp.rootSignature == nullptr)
        return;
    if (compute)
        list->SetComputeRootSignature(bp.rootSignature);
    else
        list->SetGraphicsRootSignature(bp.rootSignature);

    for (UINT i = 0; i < BindPointState::kMaxRootParams; ++i)
    {
        const BindPointState::Arg& a = bp.args[i];
        switch (a.type)
        {
        case BindPointState::ArgTable:
            if (compute)
                list->SetComputeRootDescriptorTable(i, D3D12_GPU_DESCRIPTOR_HANDLE{ a.value });
            else
                list->SetGraphicsRootDescriptorTable(i, D3D12_GPU_DESCRIPTOR_HANDLE{ a.value });
            break;
        case BindPointState::ArgCBV:
            if (compute)
                list->SetComputeRootConstantBufferView(i, a.value);
            else
                list->SetGraphicsRootConstantBufferView(i, a.value);
            break;
        case BindPointState::ArgSRV:
            if (compute)
                list->SetComputeRootShaderResourceView(i, a.value);
            else
                list->SetGraphicsRootShaderResourceView(i, a.value);
            break;
        case BindPointState::ArgUAV:
            if (compute)
                list->SetComputeRootUnorderedAccessView(i, a.value);
            else
                list->SetGraphicsRootUnorderedAccessView(i, a.value);
            break;
        default:
            break;
        }
    }
    for (const BindPointState::ConstEntry& c : bp.constants)
    {
        if (compute)
            list->SetComputeRoot32BitConstant(c.param, c.value, c.offset);
        else
            list->SetGraphicsRoot32BitConstant(c.param, c.value, c.offset);
    }
}

void RestoreState(ID3D12GraphicsCommandList* list, const ListState& s)
{
    if (s.heapCount > 0)
    {
        ID3D12DescriptorHeap* heaps[2] = { s.heaps[0], s.heaps[1] };
        list->SetDescriptorHeaps(s.heapCount, heaps);
    }
    RestoreBindPoint(list, s.compute, true);
    RestoreBindPoint(list, s.graphics, false);

    if (s.pipelineKind == ListState::PipelineKind::Pso && s.pso != nullptr)
    {
        list->SetPipelineState(s.pso);
    }
    else if (s.pipelineKind == ListState::PipelineKind::StateObject && s.stateObject != nullptr)
    {
        ID3D12GraphicsCommandList4* list4 = nullptr;
        if (SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&list4))))
        {
            list4->SetPipelineState1(s.stateObject);
            list4->Release();
        }
    }
}

} // namespace rtsky::track
