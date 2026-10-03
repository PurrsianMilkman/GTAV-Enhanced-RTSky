// RTSky - renderer: GPU resources, pipelines and the two injections
#include "Renderer.h"

#include "Barriers.h"
#include "Calibration.h"
#include "GpuLifetime.h"
#include "RtPipeline.h"
#include "ShaderBlobs.h"

#include "../../shaders/RTSkyShared.h"
#include "../Common/Config.h"
#include "../Common/D3D12Compat.h"
#include "../Common/Log.h"
#include "../Common/Math.h"
#include "../Game/GameData.h"
#include "../Game/SunModel.h"
#include "../Game/Weather.h"
#include "../Hooks/Bypass.h"
#include "../Hooks/D3D12Hooks.h"
#include "../Track/FrameAnalyzer.h"
#include "../Track/ShaderCapture.h"
#include "../Track/TlasTracker.h"

#include <d3dx12.h>
#include <wrl/client.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

namespace rtsky::render {
namespace {

using Microsoft::WRL::ComPtr;
using track::BindingRecord;
using track::ListState;

// -------------------------------------------------------------------------------------------------
// Descriptor heap layout (one shader-visible CBV_SRV_UAV heap owned by RTSky)
//   [dynamic slots]  kSlotCount tables, one per injection in flight (game-owned views)
//   [global tables]  atmosphere passes
//   [set regions]    kSetRegions x (ST_Count tables) for resolution-dependent resources
// Every table is 12 SRV descriptors followed by 8 UAV descriptors (root params 3 and 4).
// -------------------------------------------------------------------------------------------------
constexpr uint32_t kTableSrv = 12;
constexpr uint32_t kTableUav = 8;
constexpr uint32_t kTableSize = kTableSrv + kTableUav;
constexpr uint32_t kSlotCount = 64;
constexpr uint32_t kGlobalBase = kSlotCount * kTableSize;

enum GlobalTable : uint32_t { GT_Transmittance, GT_MultiScatter, GT_SkyView, GT_SkyProject, GT_Count };
constexpr uint32_t kSetBase = kGlobalBase + GT_Count * kTableSize;

enum SetTable : uint32_t
{
    ST_PrepareUav,                 // + parity
    ST_Trace = ST_PrepareUav + 2,
    ST_Temporal = ST_Trace + 2,
    ST_AtrousHist = ST_Temporal + 2,
    ST_AtrousF0toF1 = ST_AtrousHist + 2,
    ST_AtrousF1toF0 = ST_AtrousF0toF1 + 2,
    ST_CompHist = ST_AtrousF1toF0 + 2,
    ST_CompF0 = ST_CompHist + 2,
    ST_CompF1 = ST_CompF0 + 2,
    ST_Probe = ST_CompF1 + 2,
    ST_Count = ST_Probe + 2,
};
constexpr uint32_t kSetRegions = 3;
constexpr uint32_t kSetRegionSize = ST_Count * kTableSize;
// One acceleration-structure SRV per injection slot (rewritten at submit time, see OnSubmit) and a
// null one for passes that do not trace.
constexpr uint32_t kTlasBase = kSetBase + kSetRegions * kSetRegionSize;
constexpr uint32_t kTlasNull = kTlasBase + kSlotCount;
constexpr uint32_t kHeapSize = kTlasNull + 1;

constexpr uint32_t kConstantSlotSize = 1024;
constexpr uint32_t kReadbackSlotSize = 256;
constexpr uint32_t kProbeUints = 2 * RTSKY_PROBE_HYPOTHESES + 2;

static_assert(sizeof(gpu::FrameConstants) <= kConstantSlotSize, "constants do not fit a ring slot");
static_assert(kProbeUints * sizeof(uint32_t) <= kReadbackSlotSize, "probe results do not fit a readback slot");

constexpr DXGI_FORMAT kFmtDepth = DXGI_FORMAT_R32_FLOAT;
constexpr DXGI_FORMAT kFmtNormal = DXGI_FORMAT_R16G16_SNORM;
constexpr DXGI_FORMAT kFmtColor = DXGI_FORMAT_R16G16B16A16_FLOAT;

enum class Pso : uint32_t { Transmittance, MultiScatter, SkyView, SkyProject, Prepare, TraceInline, Temporal, ATrous, Composite, Probe, DebugBlit, Count };

// -------------------------------------------------------------------------------------------------
// Helpers
// -------------------------------------------------------------------------------------------------
UINT DivUp(UINT a, UINT b) { return (a + b - 1) / b; }

// Typed SRV format for a game depth resource (or UNKNOWN if it cannot be sampled).
DXGI_FORMAT DepthSrvFormat(DXGI_FORMAT resourceFormat)
{
    switch (resourceFormat)
    {
    case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_UNORM;
    default: return DXGI_FORMAT_UNKNOWN; // typed depth formats cannot have an SRV
    }
}

bool IsUnormColor(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
        return true;
    default:
        return false;
    }
}

gpu::float4 F4(float x, float y, float z, float w) { return { x, y, z, w }; }
gpu::float4 F4(const float3& v, float w) { return { v.x, v.y, v.z, w }; }

struct CameraFrame
{
    bool valid = false;
    float3 position;
    float3 right;   // unit
    float3 up;      // unit
    float3 forward; // unit
    float tanX = 1.0f;
    float tanY = 1.0f;
    float nearZ = 0.15f;
    float farZ = 10000.0f;
    uint64_t serial = 0;
};

CameraFrame MakeCamera(const game::CameraSample& s, float aspect, float fovScale)
{
    CameraFrame c;
    c.valid = s.valid;
    c.position = s.position;
    CameraBasis b = CameraBasisFromRotation(s.rotationDeg);
    c.right = b.right;
    c.up = b.up;
    c.forward = b.forward;
    c.tanY = std::tan(0.5f * s.fovDeg * kDegToRad) * fovScale;
    c.tanX = c.tanY * aspect;
    c.nearZ = s.nearClip;
    c.farZ = s.farClip;
    c.serial = s.serial;
    return c;
}

// -------------------------------------------------------------------------------------------------
// Resolution dependent resources
// -------------------------------------------------------------------------------------------------
class RendererImpl;

struct ResourceSet
{
    RendererImpl* owner = nullptr;
    uint32_t region = 0;
    UINT width = 0;
    UINT height = 0;
    ComPtr<ID3D12Resource> linearDepth[2], normal[2];
    ComPtr<ID3D12Resource> traceS, traceU;
    ComPtr<ID3D12Resource> histS[2], histU[2], histMeta[2];
    ComPtr<ID3D12Resource> filtS[2], filtU[2];
    ComPtr<ID3D12Resource> debugView; // the current debug view at trace resolution (drawn late by DebugBlit)
    ~ResourceSet();
};

struct SceneCopy
{
    ComPtr<ID3D12Resource> resource;
    D3D12_RESOURCE_DESC desc = {};
};

// Data captured at Prepare and consumed by the following Composite
struct PendingFrame
{
    bool valid = false;
    uint64_t prepareSerial = 0;
    uint32_t parity = 0;
    std::shared_ptr<ResourceSet> set;
    CameraFrame camera;
    CameraFrame hypotheses[Calibration::kLatencies];
    bool informative = false;
    int latencyUsed = 0;
    uint32_t depthMode = RTSKY_DEPTH_REVERSED_FINITE;
    UINT traceW = 0, traceH = 0;
    float depthVpX = 0.0f, depthVpY = 0.0f;
    UINT64 depthTexW = 0;
    UINT depthTexH = 0;
    ULONGLONG recordedAt = 0; // GetTickCount64 at Prepare: a Composite never pairs with an old one
};

// -------------------------------------------------------------------------------------------------
// Renderer
// -------------------------------------------------------------------------------------------------
class RendererImpl
{
public:
    bool EnsureReady(ID3D12GraphicsCommandList* list);
    void Prepare(ID3D12GraphicsCommandList* list, ListState& state, const BindingRecord& record, const Config& cfg);
    void Composite(ID3D12GraphicsCommandList* list, ListState& state, const BindingRecord& record, const Config& cfg);
    // Draws the current debug view into the game's final image (after PS_LensDistortion, before the UI).
    void DebugBlit(ID3D12GraphicsCommandList* list, ListState& state, const BindingRecord& record, const Config& cfg);
    // m_lock held. Starts a Prepare group: camera, PendingFrame, constants slot, depth SRV. Returns
    // false (with m_lastSkip set) when this frame cannot be prepared.
    bool BeginPrepareGroup(const BindingRecord& record, ID3D12Resource* depth, DXGI_FORMAT srvFormat, UINT vpX, UINT vpY, UINT w,
                           UINT h, const Config& cfg);
    std::string Status();
    std::vector<std::string> OverlayLines(const Config& cfg);
    void ResetCalibration() { m_calibration.Reset(); }
    void OnSubmit(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists);
    void RequestReset() { m_resetRequested.store(true); }
    void ReleaseRegion(uint32_t region);
    void ScriptTick();

private:
    enum class InitState : int { NotStarted, Running, Ready, Failed };

    bool Initialize(ID3D12Device5* device);
    bool CreateRootSignature();
    bool CreatePipelines();
    bool CreateGlobalResources();
    ComPtr<ID3D12Resource> CreateTexture(UINT w, UINT h, DXGI_FORMAT fmt, const wchar_t* name);
    ComPtr<ID3D12Resource> CreateBuffer(UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state, bool uav, const wchar_t* name);
    std::shared_ptr<ResourceSet> CreateSet(UINT w, UINT h);
    // A UAV-capable copy of a game texture (same desc), cached in `cache`; allocation failures back off 5 s.
    bool EnsureCopy(std::shared_ptr<SceneCopy>& cache, ULONGLONG& retryAt, const D3D12_RESOURCE_DESC& gameDesc, const wchar_t* name,
                    std::shared_ptr<SceneCopy>* out);

    // Descriptors
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(uint32_t index) const { return { m_heapCpu.ptr + SIZE_T(index) * m_increment }; }
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(uint32_t index) const { return { m_heapGpu.ptr + UINT64(index) * m_increment }; }
    void SrvTex(uint32_t index, ID3D12Resource* r, DXGI_FORMAT fmt);
    void UavTex(uint32_t index, ID3D12Resource* r, DXGI_FORMAT fmt);
    void SrvStructured(uint32_t index, ID3D12Resource* r, UINT stride, UINT count);
    void UavStructured(uint32_t index, ID3D12Resource* r, UINT stride, UINT count);
    void NullTable(uint32_t base);
    uint32_t SetTableBase(const ResourceSet& set, uint32_t table) const { return kSetBase + set.region * kSetRegionSize + table * kTableSize; }
    uint32_t SlotTableBase(uint32_t slot) const { return slot * kTableSize; }
    void WriteSetTables(const ResourceSet& set);

    // Slots
    std::shared_ptr<void> ClaimSlot(uint32_t* outSlot, uint32_t expectedFrame, bool probe, bool informative);
    void OnSlotReleased(uint32_t slot);

    // Recording helpers
    void BeginPasses(ID3D12GraphicsCommandList* list, uint32_t slot, uint32_t tlasDescriptor);
    void TlasSrv(uint32_t index, D3D12_GPU_VIRTUAL_ADDRESS address);
    void Run(ID3D12GraphicsCommandList* list, Pso pso, uint32_t srvBase, uint32_t uavBase, UINT gx, UINT gy, UINT gz, const gpu::PassConstants* pc = nullptr);
    void FillConstants(gpu::FrameConstants& fc, const PendingFrame& f, const CameraFrame* prev, const Config& cfg, bool reset,
                       int tlasSpace, const float3& camPosForTlas, UINT targetX, UINT targetY, UINT targetW, UINT targetH, bool srgbTarget);
    bool FormatSupportsTypedUav(DXGI_FORMAT f);
    bool FormatSupportsTypedStore(DXGI_FORMAT f);

    // Our resource states inside one injection
    struct OwnTracker
    {
        struct Entry { ID3D12Resource* r; D3D12_RESOURCE_STATES s; D3D12_RESOURCE_STATES rest; };
        std::vector<Entry> entries;
        OwnStates barriers;
        void Use(ID3D12Resource* r, D3D12_RESOURCE_STATES s, D3D12_RESOURCE_STATES initial = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
        {
            for (Entry& e : entries)
            {
                if (e.r == r)
                {
                    if (e.s != s)
                        barriers.Transition(r, e.s, s);
                    else if (s == D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                        barriers.Uav(r);
                    e.s = s;
                    return;
                }
            }
            entries.push_back({ r, s, initial });
            if (initial != s)
                barriers.Transition(r, initial, s);
        }
        void Flush(ID3D12GraphicsCommandList* list) { barriers.Flush(list); }
        // Textures back to their resting state; buffers are left (they decay to COMMON after ECL).
        void Rest(ID3D12GraphicsCommandList* list)
        {
            for (Entry& e : entries)
            {
                if (e.rest == D3D12_RESOURCE_STATE_COMMON)
                    continue;
                if (e.s != e.rest)
                    barriers.Transition(e.r, e.s, e.rest);
                e.s = e.rest;
            }
            barriers.Flush(list);
        }
    };

    std::atomic<InitState> m_init{ InitState::NotStarted };
    SRWLOCK m_lock = SRWLOCK_INIT;

    ComPtr<ID3D12Device5> m_device;
    D3D12_RAYTRACING_TIER m_rtTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12PipelineState> m_pso[static_cast<size_t>(Pso::Count)];
    RtPipeline m_rtPipeline;
    ComPtr<ID3D12DescriptorHeap> m_heap;
    D3D12_CPU_DESCRIPTOR_HANDLE m_heapCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE m_heapGpu = {};
    UINT m_increment = 0;

    ComPtr<ID3D12Resource> m_transmittance, m_multiScatter, m_skyView, m_skyData, m_probeBuffer;
    ComPtr<ID3D12Resource> m_constants, m_readback;
    uint8_t* m_constantsMapped = nullptr;
    const uint8_t* m_readbackMapped = nullptr;

    std::vector<uint32_t> m_freeSlots;
    struct SlotInfo { uint32_t expectedFrame = 0; bool probe = false; bool informative = false; };
    SlotInfo m_slotInfo[kSlotCount];
    SRWLOCK m_slotLock = SRWLOCK_INIT;
    std::vector<uint32_t> m_freeRegions;
    SRWLOCK m_regionLock = SRWLOCK_INIT;

    std::shared_ptr<ResourceSet> m_set;
    std::shared_ptr<SceneCopy> m_sceneCopy;
    std::shared_ptr<SceneCopy> m_blitCopy; // the final image, for DebugBlit when it has no UAV flag
    PendingFrame m_pending;
    // Prepare group: every Prepare recorded since the last Composite belongs to one frame. The
    // G-buffer can be recorded in several parallel lists with a Prepare each (gbufferEvery); they
    // share m_pending (parity, camera), this constants slot and its depth SRV, so whichever runs last
    // on the GPU - after all G-buffer draws - leaves the complete depth, whatever the recording order.
    std::shared_ptr<void> m_groupSlotHandle;
    uint32_t m_groupSlot = 0;
    ComPtr<ID3D12Resource> m_groupDepth; // the depth the slot's SRV views (kept alive with it)
    uint64_t m_prepareSerial = 0;
    uint64_t m_lastCompositePrepareSerial = 0;
    CameraFrame m_lastCompositeCamera;
    const ResourceSet* m_lastCompositeSet = nullptr;
    UINT m_lastCompositeTraceW = 0;
    UINT m_lastCompositeTraceH = 0;
    int m_lastDebugView = -1;
    ULONGLONG m_setRetryAt = 0;      // screen-resource allocation backoff (GetTickCount64)
    ULONGLONG m_sceneCopyRetryAt = 0; // scene-copy allocation backoff
    ULONGLONG m_blitCopyRetryAt = 0;
    std::string m_blitSkip;           // why the last DebugBlit was not recorded ("" = it was)
    uint64_t m_lastTlasSerial = 0;
    int m_tlasReuse = 0;
    uint32_t m_frameIndex = 0;
    std::atomic<bool> m_resetRequested{ true };
    std::vector<std::pair<DXGI_FORMAT, bool>> m_typedUavCache;

    Calibration m_calibration;

    // statistics
    std::atomic<uint64_t> m_prepares{ 0 };
    std::atomic<ULONGLONG> m_lastRelitTick{ 0 }; // last Composite that relit the image
    std::atomic<ULONGLONG> m_lastHeldTick{ 0 };  // last Composite held back by the calibration gate
    std::atomic<ULONGLONG> m_lastBlitTick{ 0 };  // last DebugBlit recorded into the final image
    std::atomic<uint64_t> m_lastExecutedPrepare{ 0 };
    std::atomic<uint64_t> m_pairingLag{ 0 };
    std::atomic<uint64_t> m_lateTlas{ 0 }; // composites upgraded to a newer TLAS at submission
    std::atomic<uint64_t> m_composites{ 0 };
    std::string m_lastSkip;
    ULONGLONG m_lastStatusLog = 0;
};

RendererImpl& Instance()
{
    // Never destroyed: hooks and GPU-lifetime deleters may still run during process exit, after
    // static destructors (destruction order across translation units is unspecified).
    static RendererImpl* instance = new RendererImpl();
    return *instance;
}

ResourceSet::~ResourceSet()
{
    if (owner != nullptr)
        owner->ReleaseRegion(region);
}

// -------------------------------------------------------------------------------------------------
// Initialisation
// -------------------------------------------------------------------------------------------------
bool RendererImpl::EnsureReady(ID3D12GraphicsCommandList* list)
{
    InitState s = m_init.load(std::memory_order_acquire);
    if (s == InitState::Ready)
    {
        // Everything RTSky records references objects of one device. A list of another device (the
        // game recreated its device) or a removed device ends RTSky for this session.
        ComPtr<ID3D12Device> listDevice;
        if (FAILED(list->GetDevice(IID_PPV_ARGS(&listDevice))) || listDevice.Get() != static_cast<ID3D12Device*>(m_device.Get()))
        {
            RTSKY_LOG_ONCE(log::Level::Warning, "Command list of another D3D12 device (device recreated?): RTSky stops injecting until the game restarts");
            return false;
        }
        if (m_device->GetDeviceRemovedReason() != S_OK)
        {
            LOG_ERROR("The D3D12 device was removed (0x%08X): RTSky stops injecting", static_cast<unsigned>(m_device->GetDeviceRemovedReason()));
            m_init.store(InitState::Failed);
            return false;
        }
        return true;
    }
    if (s != InitState::NotStarted)
        return false;

    ComPtr<ID3D12Device5> device;
    if (FAILED(list->GetDevice(IID_PPV_ARGS(&device))))
    {
        RTSKY_LOG_ONCE(log::Level::Error, "The game's device does not support ID3D12Device5 (DXR)");
        m_init.store(InitState::Failed);
        return false;
    }
    InitState expected = InitState::NotStarted;
    if (!m_init.compare_exchange_strong(expected, InitState::Running))
        return false;

    // Pipeline creation (DXR state object compilation) can take tens of milliseconds: never on the
    // game's recording thread.
    std::thread([this, device]() {
        hooks::HookBypass bypass;
        bool ok = Initialize(device.Get());
        m_init.store(ok ? InitState::Ready : InitState::Failed, std::memory_order_release);
        if (ok)
            LOG_INFO("Renderer ready");
        else
            LOG_ERROR("Renderer initialisation failed - RTSky stays inactive");
    }).detach();
    return false;
}

bool RendererImpl::Initialize(ID3D12Device5* device)
{
    m_device = device;

    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5))) ||
        options5.RaytracingTier < D3D12_RAYTRACING_TIER_1_1)
    {
        LOG_ERROR("Ray tracing tier 1.1 is required (device reports %d)", static_cast<int>(options5.RaytracingTier));
        return false;
    }
    m_rtTier = options5.RaytracingTier;

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = kHeapSize;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_heap))))
    {
        LOG_ERROR("CreateDescriptorHeap failed");
        return false;
    }
    m_heap->SetName(L"RTSky descriptors");
    m_heapCpu = HeapCpuStart(m_heap.Get());
    m_heapGpu = HeapGpuStart(m_heap.Get());
    m_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    if (!CreateRootSignature() || !CreatePipelines() || !CreateGlobalResources())
        return false;

    for (uint32_t i = 0; i + kTableSize <= kTlasBase; i += kTableSize)
        NullTable(i);
    for (uint32_t i = kTlasBase; i <= kTlasNull; ++i)
        TlasSrv(i, 0); // null acceleration structure: every ray misses

    // Global tables
    NullTable(kGlobalBase + GT_Transmittance * kTableSize);
    UavTex(kGlobalBase + GT_Transmittance * kTableSize + kTableSrv, m_transmittance.Get(), kFmtColor);
    SrvTex(kGlobalBase + GT_MultiScatter * kTableSize, m_transmittance.Get(), kFmtColor);
    UavTex(kGlobalBase + GT_MultiScatter * kTableSize + kTableSrv, m_multiScatter.Get(), kFmtColor);
    SrvTex(kGlobalBase + GT_SkyView * kTableSize, m_transmittance.Get(), kFmtColor);
    SrvTex(kGlobalBase + GT_SkyView * kTableSize + 1, m_multiScatter.Get(), kFmtColor);
    UavTex(kGlobalBase + GT_SkyView * kTableSize + kTableSrv, m_skyView.Get(), kFmtColor);
    SrvTex(kGlobalBase + GT_SkyProject * kTableSize, m_skyView.Get(), kFmtColor);
    SrvTex(kGlobalBase + GT_SkyProject * kTableSize + 1, m_transmittance.Get(), kFmtColor);
    UavStructured(kGlobalBase + GT_SkyProject * kTableSize + kTableSrv, m_skyData.Get(), sizeof(gpu::SkyData), 1);

    AcquireSRWLockExclusive(&m_slotLock);
    for (uint32_t i = kSlotCount; i > 0; --i)
        m_freeSlots.push_back(i - 1);
    ReleaseSRWLockExclusive(&m_slotLock);
    AcquireSRWLockExclusive(&m_regionLock);
    for (uint32_t i = kSetRegions; i > 0; --i)
        m_freeRegions.push_back(i - 1);
    ReleaseSRWLockExclusive(&m_regionLock);
    return true;
}

bool RendererImpl::CreateRootSignature()
{
    using PFN_Serialize = HRESULT(WINAPI*)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob**);
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    auto serialize = d3d12 != nullptr
        ? reinterpret_cast<PFN_Serialize>(reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12SerializeVersionedRootSignature")))
        : nullptr;
    if (serialize == nullptr)
    {
        LOG_ERROR("D3D12SerializeVersionedRootSignature not available");
        return false;
    }

    const D3D12_DESCRIPTOR_RANGE_FLAGS rangeFlags =
        D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
    CD3DX12_DESCRIPTOR_RANGE1 srvRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kTableSrv, 0, 0, rangeFlags);
    CD3DX12_DESCRIPTOR_RANGE1 uavRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kTableUav, 0, 0, rangeFlags);
    // The TLAS is a descriptor (not a root SRV) so that it can still be changed after recording, up to
    // submission (DESCRIPTORS_VOLATILE): see OnSubmit.
    CD3DX12_DESCRIPTOR_RANGE1 tlasRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 1, rangeFlags);

    CD3DX12_ROOT_PARAMETER1 params[5];
    params[0].InitAsConstantBufferView(0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE);
    params[1].InitAsDescriptorTable(1, &tlasRange);
    params[2].InitAsConstants(sizeof(gpu::PassConstants) / 4, 1, 0);
    params[3].InitAsDescriptorTable(1, &srvRange);
    params[4].InitAsDescriptorTable(1, &uavRange);

    CD3DX12_STATIC_SAMPLER_DESC samplers[2];
    samplers[0].Init(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                     D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    samplers[1].Init(1, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                     D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC desc;
    desc.Init_1_1(5, params, 2, samplers, D3D12_ROOT_SIGNATURE_FLAG_NONE);

    ComPtr<ID3DBlob> blob, error;
    HRESULT hr = serialize(&desc, &blob, &error);
    if (FAILED(hr))
    {
        LOG_ERROR("Root signature serialisation failed: %s", error ? static_cast<const char*>(error->GetBufferPointer()) : "?");
        return false;
    }
    hr = m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature));
    if (FAILED(hr))
    {
        LOG_ERROR("CreateRootSignature failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }
    m_rootSignature->SetName(L"RTSky root signature");
    return true;
}

bool RendererImpl::CreatePipelines()
{
    struct Entry { Pso pso; ShaderId shader; const wchar_t* name; };
    const Entry entries[] = {
        { Pso::Transmittance, ShaderId::Transmittance, L"RTSky Transmittance" },
        { Pso::MultiScatter, ShaderId::MultiScatter, L"RTSky MultiScatter" },
        { Pso::SkyView, ShaderId::SkyView, L"RTSky SkyView" },
        { Pso::SkyProject, ShaderId::SkyProject, L"RTSky SkyProject" },
        { Pso::Prepare, ShaderId::Prepare, L"RTSky Prepare" },
        { Pso::TraceInline, ShaderId::TraceInline, L"RTSky TraceInline" },
        { Pso::Temporal, ShaderId::Temporal, L"RTSky Temporal" },
        { Pso::ATrous, ShaderId::ATrous, L"RTSky ATrous" },
        { Pso::Composite, ShaderId::Composite, L"RTSky Composite" },
        { Pso::Probe, ShaderId::Probe, L"RTSky Probe" },
        { Pso::DebugBlit, ShaderId::DebugBlit, L"RTSky DebugBlit" },
    };
    for (const Entry& e : entries)
    {
        ShaderBlob blob = GetShader(e.shader);
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = m_rootSignature.Get();
        desc.CS = { blob.data, blob.size };
        HRESULT hr = m_device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&m_pso[static_cast<size_t>(e.pso)]));
        if (FAILED(hr))
        {
            LOG_ERROR("CreateComputePipelineState failed for a shader (0x%08X)", static_cast<unsigned>(hr));
            return false;
        }
        m_pso[static_cast<size_t>(e.pso)]->SetName(e.name);
    }

    // Opacity micromaps can only exist in the game's BLASes on tier 1.2 hardware; allow them there so
    // that traversal is defined if the game uses them.
    const bool allowOmm = m_rtTier >= D3D12_RAYTRACING_TIER_1_2;
    ShaderBlob lib = GetShader(ShaderId::TraceLibrary);
    if (!m_rtPipeline.Create(m_device.Get(), m_rootSignature.Get(), lib.data, lib.size, allowOmm))
        LOG_WARN("DXR pipeline unavailable, the inline (RayQuery) trace path will be used");
    return true;
}

ComPtr<ID3D12Resource> RendererImpl::CreateTexture(UINT w, UINT h, DXGI_FORMAT fmt, const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = w;
    desc.Height = h;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = fmt;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> r;
    // Resting state of every RTSky texture between passes and between frames.
    if (FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                 nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    r->SetName(name);
    return r;
}

ComPtr<ID3D12Resource> RendererImpl::CreateBuffer(UINT64 size, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_STATES state, bool uav,
                                                   const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = heapType;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> r;
    if (FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    r->SetName(name);
    return r;
}

bool RendererImpl::CreateGlobalResources()
{
    m_transmittance = CreateTexture(RTSKY_TRANSMITTANCE_W, RTSKY_TRANSMITTANCE_H, kFmtColor, L"RTSky transmittance LUT");
    m_multiScatter = CreateTexture(RTSKY_MULTISCATTER_SIZE, RTSKY_MULTISCATTER_SIZE, kFmtColor, L"RTSky multiple scattering LUT");
    m_skyView = CreateTexture(RTSKY_SKYVIEW_W, RTSKY_SKYVIEW_H, kFmtColor, L"RTSky sky view LUT");
    // Buffers are always created in COMMON and decay back to COMMON after every ExecuteCommandLists.
    m_skyData = CreateBuffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, true, L"RTSky sky data");
    m_probeBuffer = CreateBuffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, true, L"RTSky probe results");
    m_constants = CreateBuffer(UINT64(kSlotCount) * kConstantSlotSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false,
                               L"RTSky constants");
    m_readback = CreateBuffer(UINT64(kSlotCount) * kReadbackSlotSize, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false,
                              L"RTSky probe readback");
    if (!m_transmittance || !m_multiScatter || !m_skyView || !m_skyData || !m_probeBuffer || !m_constants || !m_readback)
    {
        LOG_ERROR("Global resource allocation failed");
        return false;
    }
    D3D12_RANGE noRead = { 0, 0 };
    if (FAILED(m_constants->Map(0, &noRead, reinterpret_cast<void**>(&m_constantsMapped))))
        return false;
    void* rb = nullptr;
    D3D12_RANGE all = { 0, SIZE_T(kSlotCount) * kReadbackSlotSize };
    if (FAILED(m_readback->Map(0, &all, &rb)))
        return false;
    m_readbackMapped = static_cast<const uint8_t*>(rb);
    return true;
}

std::shared_ptr<ResourceSet> RendererImpl::CreateSet(UINT w, UINT h)
{
    uint32_t region = UINT32_MAX;
    AcquireSRWLockExclusive(&m_regionLock);
    if (!m_freeRegions.empty())
    {
        region = m_freeRegions.back();
        m_freeRegions.pop_back();
    }
    ReleaseSRWLockExclusive(&m_regionLock);
    if (region == UINT32_MAX)
        return nullptr; // previous sets still in flight; try again next frame

    auto set = std::make_shared<ResourceSet>();
    set->owner = this;
    set->region = region;
    set->width = w;
    set->height = h;
    // Stop at the first failure (out of video memory): trying the remaining full-screen textures
    // would only make it worse.
    bool ok = true;
    auto make = [&](ComPtr<ID3D12Resource>& r, DXGI_FORMAT fmt, const wchar_t* name) {
        if (ok)
            ok = (r = CreateTexture(w, h, fmt, name)) != nullptr;
    };
    for (int i = 0; i < 2; ++i)
    {
        make(set->linearDepth[i], kFmtDepth, L"RTSky linear depth");
        make(set->normal[i], kFmtNormal, L"RTSky normal");
        make(set->histS[i], kFmtColor, L"RTSky history S");
        make(set->histU[i], kFmtColor, L"RTSky history U");
        make(set->histMeta[i], kFmtColor, L"RTSky history meta");
        make(set->filtS[i], kFmtColor, L"RTSky filter S");
        make(set->filtU[i], kFmtColor, L"RTSky filter U");
    }
    make(set->traceS, kFmtColor, L"RTSky trace S");
    make(set->traceU, kFmtColor, L"RTSky trace U");
    make(set->debugView, kFmtColor, L"RTSky debug view");
    if (!ok)
    {
        // Retry in 5 s, and only log the first failure of a streak.
        if (m_setRetryAt == 0 || GetTickCount64() > m_setRetryAt + 60000)
            LOG_ERROR("Allocation of %ux%u screen resources failed (out of video memory?) - retrying every 5 s", w, h);
        m_setRetryAt = GetTickCount64() + 5000;
        return nullptr; // destructor returns the region
    }
    m_setRetryAt = 0;
    WriteSetTables(*set);
    LOG_INFO("Screen resources created: %ux%u (descriptor region %u)", w, h, region);
    return set;
}

void RendererImpl::ReleaseRegion(uint32_t region)
{
    AcquireSRWLockExclusive(&m_regionLock);
    m_freeRegions.push_back(region);
    ReleaseSRWLockExclusive(&m_regionLock);
}

bool RendererImpl::EnsureCopy(std::shared_ptr<SceneCopy>& cache, ULONGLONG& retryAt, const D3D12_RESOURCE_DESC& gameDesc,
                              const wchar_t* name, std::shared_ptr<SceneCopy>* out)
{
    if (cache && cache->desc.Width == gameDesc.Width && cache->desc.Height == gameDesc.Height && cache->desc.Format == gameDesc.Format &&
        cache->desc.MipLevels == gameDesc.MipLevels && cache->desc.DepthOrArraySize == gameDesc.DepthOrArraySize)
    {
        *out = cache;
        return true;
    }
    if (GetTickCount64() < retryAt)
        return false;
    D3D12_RESOURCE_DESC desc = gameDesc;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    desc.Alignment = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    auto copy = std::make_shared<SceneCopy>();
    if (FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                 nullptr, IID_PPV_ARGS(&copy->resource))))
    {
        if (retryAt == 0)
            LOG_ERROR("%ls allocation failed (out of video memory?) - retrying every 5 s", name);
        retryAt = GetTickCount64() + 5000;
        return false;
    }
    copy->resource->SetName(name);
    copy->desc = gameDesc;
    cache = copy; // in-flight users keep the previous one alive through their attachments
    *out = copy;
    return true;
}

bool RendererImpl::FormatSupportsTypedStore(DXGI_FORMAT f)
{
    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = {};
    fs.Format = f;
    return SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs))) &&
           (fs.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) != 0 && (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
}

// -------------------------------------------------------------------------------------------------
// Descriptors
// -------------------------------------------------------------------------------------------------
void RendererImpl::SrvTex(uint32_t index, ID3D12Resource* r, DXGI_FORMAT fmt)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
    d.Format = fmt;
    d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.Texture2D.MipLevels = 1;
    m_device->CreateShaderResourceView(r, &d, Cpu(index));
}

void RendererImpl::UavTex(uint32_t index, ID3D12Resource* r, DXGI_FORMAT fmt)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
    d.Format = fmt;
    d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m_device->CreateUnorderedAccessView(r, nullptr, &d, Cpu(index));
}

void RendererImpl::SrvStructured(uint32_t index, ID3D12Resource* r, UINT stride, UINT count)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
    d.Format = DXGI_FORMAT_UNKNOWN;
    d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.Buffer.NumElements = count;
    d.Buffer.StructureByteStride = stride;
    m_device->CreateShaderResourceView(r, &d, Cpu(index));
}

void RendererImpl::UavStructured(uint32_t index, ID3D12Resource* r, UINT stride, UINT count)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
    d.Format = DXGI_FORMAT_UNKNOWN;
    d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    d.Buffer.NumElements = count;
    d.Buffer.StructureByteStride = stride;
    m_device->CreateUnorderedAccessView(r, nullptr, &d, Cpu(index));
}

void RendererImpl::NullTable(uint32_t base)
{
    for (uint32_t i = 0; i < kTableSrv; ++i)
        SrvTex(base + i, nullptr, DXGI_FORMAT_R8G8B8A8_UNORM);
    for (uint32_t i = 0; i < kTableUav; ++i)
        UavTex(base + kTableSrv + i, nullptr, DXGI_FORMAT_R32_FLOAT);
}

void RendererImpl::WriteSetTables(const ResourceSet& s)
{
    for (uint32_t t = 0; t < ST_Count; ++t)
        NullTable(SetTableBase(s, t));

    const UINT skyStride = sizeof(gpu::SkyData);
    for (uint32_t p = 0; p < 2; ++p)
    {
        const uint32_t q = 1 - p;
        uint32_t b;

        b = SetTableBase(s, ST_PrepareUav + p);
        UavTex(b + kTableSrv + 0, s.linearDepth[p].Get(), kFmtDepth);
        UavTex(b + kTableSrv + 1, s.normal[p].Get(), kFmtNormal);

        b = SetTableBase(s, ST_Trace + p);
        SrvTex(b + 0, s.linearDepth[p].Get(), kFmtDepth);
        SrvTex(b + 1, s.normal[p].Get(), kFmtNormal);
        SrvTex(b + 2, m_skyView.Get(), kFmtColor);
        SrvStructured(b + 3, m_skyData.Get(), skyStride, 1);
        UavTex(b + kTableSrv + 0, s.traceS.Get(), kFmtColor);
        UavTex(b + kTableSrv + 1, s.traceU.Get(), kFmtColor);

        b = SetTableBase(s, ST_Temporal + p);
        SrvTex(b + 0, s.traceS.Get(), kFmtColor);
        SrvTex(b + 1, s.traceU.Get(), kFmtColor);
        SrvTex(b + 2, s.linearDepth[p].Get(), kFmtDepth);
        SrvTex(b + 3, s.normal[p].Get(), kFmtNormal);
        SrvTex(b + 4, s.linearDepth[q].Get(), kFmtDepth);
        SrvTex(b + 5, s.normal[q].Get(), kFmtNormal);
        SrvTex(b + 6, s.histS[q].Get(), kFmtColor);
        SrvTex(b + 7, s.histU[q].Get(), kFmtColor);
        SrvTex(b + 8, s.histMeta[q].Get(), kFmtColor);
        UavTex(b + kTableSrv + 0, s.histS[p].Get(), kFmtColor);
        UavTex(b + kTableSrv + 1, s.histU[p].Get(), kFmtColor);
        UavTex(b + kTableSrv + 2, s.histMeta[p].Get(), kFmtColor);

        auto atrous = [&](uint32_t table, ID3D12Resource* inS, ID3D12Resource* inU, ID3D12Resource* outS, ID3D12Resource* outU) {
            uint32_t base = SetTableBase(s, table + p);
            SrvTex(base + 0, inS, kFmtColor);
            SrvTex(base + 1, inU, kFmtColor);
            SrvTex(base + 2, s.linearDepth[p].Get(), kFmtDepth);
            SrvTex(base + 3, s.normal[p].Get(), kFmtNormal);
            UavTex(base + kTableSrv + 0, outS, kFmtColor);
            UavTex(base + kTableSrv + 1, outU, kFmtColor);
        };
        atrous(ST_AtrousHist, s.histS[p].Get(), s.histU[p].Get(), s.filtS[0].Get(), s.filtU[0].Get());
        atrous(ST_AtrousF0toF1, s.filtS[0].Get(), s.filtU[0].Get(), s.filtS[1].Get(), s.filtU[1].Get());
        atrous(ST_AtrousF1toF0, s.filtS[1].Get(), s.filtU[1].Get(), s.filtS[0].Get(), s.filtU[0].Get());

        auto composite = [&](uint32_t table, ID3D12Resource* srcS, ID3D12Resource* srcU) {
            uint32_t base = SetTableBase(s, table + p);
            SrvTex(base + 0, srcS, kFmtColor);
            SrvTex(base + 1, srcU, kFmtColor);
            SrvTex(base + 2, s.linearDepth[p].Get(), kFmtDepth);
            SrvTex(base + 3, s.normal[p].Get(), kFmtNormal);
            SrvStructured(base + 4, m_skyData.Get(), skyStride, 1);
            SrvTex(base + 5, s.histMeta[p].Get(), kFmtColor);
            SrvTex(base + 6, s.traceS.Get(), kFmtColor);
        };
        composite(ST_CompHist, s.histS[p].Get(), s.histU[p].Get());
        composite(ST_CompF0, s.filtS[0].Get(), s.filtU[0].Get());
        composite(ST_CompF1, s.filtS[1].Get(), s.filtU[1].Get());

        b = SetTableBase(s, ST_Probe + p);
        SrvTex(b + 0, s.linearDepth[p].Get(), kFmtDepth);
        UavStructured(b + kTableSrv + 0, m_probeBuffer.Get(), sizeof(uint32_t), 64);
    }
}

// -------------------------------------------------------------------------------------------------
// Slots (constants + dynamic descriptors + probe readback per injection)
// -------------------------------------------------------------------------------------------------
std::shared_ptr<void> RendererImpl::ClaimSlot(uint32_t* outSlot, uint32_t expectedFrame, bool probe, bool informative)
{
    uint32_t slot = UINT32_MAX;
    AcquireSRWLockExclusive(&m_slotLock);
    if (!m_freeSlots.empty())
    {
        slot = m_freeSlots.back();
        m_freeSlots.pop_back();
        m_slotInfo[slot] = { expectedFrame, probe, informative };
    }
    ReleaseSRWLockExclusive(&m_slotLock);
    if (slot == UINT32_MAX)
        return {};
    *outSlot = slot;
    RendererImpl* self = this;
    return std::shared_ptr<void>(reinterpret_cast<void*>(uintptr_t(slot) + 1), [self, slot](void*) { self->OnSlotReleased(slot); });
}

void RendererImpl::OnSlotReleased(uint32_t slot)
{
    SlotInfo info;
    AcquireSRWLockShared(&m_slotLock);
    info = m_slotInfo[slot];
    ReleaseSRWLockShared(&m_slotLock);

    // Probe results: only valid if the GPU actually executed this slot's commands, which the frame
    // stamp written by ProbeCS proves.
    if (info.probe && m_readbackMapped != nullptr)
    {
        uint32_t results[kProbeUints];
        std::memcpy(results, m_readbackMapped + SIZE_T(slot) * kReadbackSlotSize, sizeof(results));
        if (results[kProbeUints - 1] == info.expectedFrame && results[2 * RTSKY_PROBE_HYPOTHESES] > 0)
            m_calibration.Submit(results, info.informative);
    }

    AcquireSRWLockExclusive(&m_slotLock);
    m_freeSlots.push_back(slot);
    ReleaseSRWLockExclusive(&m_slotLock);
}

// -------------------------------------------------------------------------------------------------
// Recording helpers
// -------------------------------------------------------------------------------------------------
void RendererImpl::TlasSrv(uint32_t index, D3D12_GPU_VIRTUAL_ADDRESS address)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
    d.Format = DXGI_FORMAT_UNKNOWN;
    d.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.RaytracingAccelerationStructure.Location = address;
    hooks::HookBypass bypass;
    m_device->CreateShaderResourceView(nullptr, &d, Cpu(index));
}

void RendererImpl::BeginPasses(ID3D12GraphicsCommandList* list, uint32_t slot, uint32_t tlasDescriptor)
{
    ID3D12DescriptorHeap* heaps[] = { m_heap.Get() };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(m_rootSignature.Get());
    list->SetComputeRootConstantBufferView(0, m_constants->GetGPUVirtualAddress() + UINT64(slot) * kConstantSlotSize);
    list->SetComputeRootDescriptorTable(1, Gpu(tlasDescriptor));
    gpu::PassConstants zero = {};
    list->SetComputeRoot32BitConstants(2, sizeof(zero) / 4, &zero, 0);
}

void RendererImpl::Run(ID3D12GraphicsCommandList* list, Pso pso, uint32_t srvBase, uint32_t uavBase, UINT gx, UINT gy, UINT gz,
                       const gpu::PassConstants* pc)
{
    list->SetPipelineState(m_pso[static_cast<size_t>(pso)].Get());
    list->SetComputeRootDescriptorTable(3, Gpu(srvBase));
    list->SetComputeRootDescriptorTable(4, Gpu(uavBase));
    if (pc != nullptr)
        list->SetComputeRoot32BitConstants(2, sizeof(*pc) / 4, pc, 0);
    list->Dispatch(gx, gy, gz);
}

bool RendererImpl::FormatSupportsTypedUav(DXGI_FORMAT f)
{
    for (const auto& [format, ok] : m_typedUavCache)
    {
        if (format == f)
            return ok;
    }
    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = {};
    fs.Format = f;
    bool ok = SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs))) &&
              (fs.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) != 0 &&
              (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) != 0 && (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
    m_typedUavCache.emplace_back(f, ok);
    return ok;
}

// Fills everything that does not depend on the pass. The camera in `f` is the one used for the
// depth reconstruction of this frame (chosen at Prepare).
void RendererImpl::FillConstants(gpu::FrameConstants& fc, const PendingFrame& f, const CameraFrame* prev, const Config& cfg, bool reset,
                                 int tlasSpace, const float3& camPosForTlas, UINT targetX, UINT targetY, UINT targetW, UINT targetH,
                                 bool srgbTarget)
{
    std::memset(&fc, 0, sizeof(fc));
    const CameraFrame& c = f.camera;
    fc.camPos = F4(c.position, 0.0f);
    fc.camRight = F4(c.right * c.tanX, c.nearZ);
    fc.camUp = F4(c.up * c.tanY, c.farZ);
    fc.camForward = F4(c.forward, static_cast<float>(f.depthMode));

    const CameraFrame& p = (prev != nullptr && prev->valid) ? *prev : c;
    fc.prevCamOffset = F4(p.position - c.position, 0.0f);
    fc.prevCamRight = F4(p.right * p.tanX, p.nearZ);
    fc.prevCamUp = F4(p.up * p.tanY, p.farZ);
    fc.prevCamForward = F4(p.forward, static_cast<float>(f.depthMode));

    fc.tlasOffset = F4(tlasSpace == 0 ? camPosForTlas : float3(0.0f, 0.0f, 0.0f), 0.0f);

    fc.traceSize = F4(float(f.traceW), float(f.traceH), 1.0f / float(f.traceW), 1.0f / float(f.traceH));
    fc.depthViewport = F4(f.depthVpX, f.depthVpY, float(f.depthTexW), float(f.depthTexH));
    fc.targetViewport = F4(float(targetX), float(targetY), float(targetW), float(targetH));

    // Sun / moon
    const game::EnvironmentSample env = game::Game().GetEnvironment();
    game::SunModelParams sp;
    sp.sunRollDeg = cfg.sunRollDeg;
    sp.dayStartHour = cfg.dayStartHour;
    sp.dayLengthHours = cfg.dayLengthHours;
    sp.azimuthOffsetDeg = cfg.sunAzimuthOffsetDeg;
    sp.moonRollDeg = cfg.moonRollDeg;
    const game::CelestialDirections dirs = game::ComputeCelestialDirections(env.valid ? env.hours : 12.0f, sp);
    const float sunRadius = 0.00467f;
    const float moonRadius = 0.0045f;
    fc.sunDir = F4(dirs.sun, sunRadius * std::max(cfg.sunSoftness, 1.0f));
    fc.moonDir = F4(dirs.moon, moonRadius * std::max(cfg.sunSoftness, 1.0f));
    const bool skyLitBySun = dirs.sun.z > -0.17f; // until ~10 deg below the horizon (twilight)
    fc.lightSelect = F4(skyLitBySun ? 1.0f : 0.0f, Saturate(dirs.sun.z * 10.0f + 0.5f), 0.0f, 0.0f);

    // Atmosphere (km) - Earth defaults of the Hillaire 2020 reference implementation
    const float altitudeKm = std::max(c.position.z, 0.0f) * 0.001f;
    fc.atmoRadii = F4(6360.0f, 6460.0f, altitudeKm, 0.8f);
    fc.rayleighScattering = F4(5.802e-3f, 13.558e-3f, 33.1e-3f, 8.0f);
    const float mie = std::max(cfg.mieScale, 0.0f);
    fc.mieParams = F4(3.996e-3f * mie, 4.440e-3f * mie, 0.444e-3f * mie, 1.2f);
    fc.ozoneAbsorption = F4(0.650e-3f, 1.881e-3f, 0.085e-3f, 25.0f);
    fc.atmoMisc = F4(30.0f, Saturate(cfg.atmosphereGroundAlbedo), 1.0f, 0.0f);
    const float sun = std::max(cfg.sunIntensity, 0.0f);
    fc.solarIlluminance = F4(sun, sun, sun, 0.0f);
    const float moon = std::max(cfg.moonIntensity, 0.0f);
    fc.moonIlluminance = F4(moon * 0.85f, moon * 0.9f, moon, 0.0f);

    const game::WeatherParams w = game::EvaluateWeather(env.weatherFrom, env.weatherTo, env.weatherBlend, env.rainLevel, env.snowLevel);
    fc.weather = F4(w.cloudiness, w.overcastTransmission, w.directFactor, w.wetness);

    // Trace
    fc.traceParams = F4(cfg.maxRayDistance, cfg.normalBias, cfg.distanceBias, cfg.tMin);
    fc.foliageParams = F4(cfg.foliageOpacity, cfg.foliageCells, cfg.nearField ? std::max(cfg.nearFieldRadius, 0.0f) : 0.0f, 0.0f);
    uint32_t flags = 0;
    if (cfg.sunShadowRays)
        flags |= RTSKY_FLAG_SUN_RAYS;
    if (cfg.foliageMode == FoliageMode::Opaque)
        flags |= RTSKY_FLAG_FOLIAGE_OPAQUE;
    else if (cfg.foliageMode == FoliageMode::Ignore)
        flags |= RTSKY_FLAG_FOLIAGE_IGNORE;
    if (cfg.debugView == RTSKY_VIEW_TLAS)
        flags |= RTSKY_FLAG_DEBUG_TLAS;
    if (f.depthMode != RTSKY_DEPTH_STANDARD)
        flags |= RTSKY_FLAG_REVERSED_Z;
    fc.traceFlags = { static_cast<uint32_t>(cfg.raysPerPixel), cfg.instanceMask, m_frameIndex, flags };

    fc.temporalParams = F4(cfg.maxHistory, cfg.depthReject, cfg.normalReject, reset ? 1.0f : 0.0f);
    fc.denoiseParams = F4(cfg.sigmaPlane, cfg.normalPower, cfg.sigmaLuminance, 0.0f);

    float fade = 1.0f;
    if (env.interior)
        fade = std::min(fade, Saturate(cfg.interiorStrength));
    if (env.cutscene)
        fade = std::min(fade, Saturate(cfg.cutsceneStrength));
    if (env.loading)
        fade = 0.0f;
    fc.compositeParams = F4(Saturate(cfg.strength), cfg.minRatio, cfg.maxRatio, std::max(cfg.gameSkyOcclusion, 0.0f));
    fc.compositeParams2 = F4(cfg.directScale, std::max(cfg.artificialAmbient, 0.0f), Saturate(cfg.groundAlbedo), 0.0f);
    fc.compositeParams3 = F4(fade, std::max(cfg.nearFadeDistance, 0.0f), static_cast<float>(cfg.debugView), srgbTarget ? 1.0f : 0.0f);
    // Debug views go only to the DebugView texture while the late blit draws them (seen within 1 s).
    const bool blitActive = GetTickCount64() - m_lastBlitTick.load(std::memory_order_relaxed) < 1000;
    fc.compositeParams4 = F4(cfg.fadeStart, cfg.fadeEnd, cfg.compareSplit ? 1.0f : 0.0f, blitActive ? 1.0f : 0.0f);

    // Calibration hypotheses: latency l (0..3) x TLAS space s (0 world, 1 camera-relative)
    for (int l = 0; l < Calibration::kLatencies; ++l)
    {
        const CameraFrame& h = f.hypotheses[l];
        for (int s = 0; s < Calibration::kSpaces; ++s)
        {
            const int k = l * Calibration::kSpaces + s;
            if (!h.valid)
            {
                fc.probeCam[k * 4 + 3] = F4(0.0f, 0.0f, 0.0f, 0.0f);
                continue;
            }
            fc.probeCam[k * 4 + 0] = F4(h.position - c.position, float(s));
            fc.probeCam[k * 4 + 1] = F4(h.right * h.tanX, 0.0f);
            fc.probeCam[k * 4 + 2] = F4(h.up * h.tanY, 0.0f);
            fc.probeCam[k * 4 + 3] = F4(h.forward, 0.0f);
        }
    }
}

// Takes over the state the game's last barrier (on this list) left subresource 0 in. False when
// RTSky cannot continue from it: unknown state, NO_ACCESS, or a split barrier that has begun but not
// ended (nothing may touch the subresource in between).
static bool StateFromObserved(const track::ObservedState& o, GameResourceState* out)
{
    if (o.splitPending)
        return false;
    out->enhanced = o.enhanced;
    const bool known = o.enhanced ? UsageFromLayout(static_cast<D3D12_BARRIER_LAYOUT>(o.stateOrLayout), &out->usage)
                                  : UsageFromLegacyState(static_cast<D3D12_RESOURCE_STATES>(o.stateOrLayout), &out->usage);
    if (!known)
        return false;
    out->legacyState = static_cast<D3D12_RESOURCE_STATES>(o.stateOrLayout);
    out->layout = static_cast<D3D12_BARRIER_LAYOUT>(o.stateOrLayout);
    if (o.enhanced)
    {
        // NO_ACCESS (with SYNC_NONE) is deliberately not taken over: the restore would have to re-open
        // a NO_ACCESS / SYNC_NONE scope, and the game's next barrier (SyncBefore NONE) would then not
        // wait for RTSky's compute work on the resource.
        if (o.accessAfter == D3D12_BARRIER_ACCESS_NO_ACCESS)
            return false;
        out->scopeObserved = true;
        out->sync = o.syncAfter;
        out->access = o.accessAfter;
    }
    return true;
}

// -------------------------------------------------------------------------------------------------
// Injection 1: Prepare (end of the G-buffer pass)
// -------------------------------------------------------------------------------------------------
void RendererImpl::Prepare(ID3D12GraphicsCommandList* list, ListState& state, const BindingRecord& record, const Config& cfg)
{
    ID3D12Resource* depth = record.dsv.resource;
    if (!record.hasDsv || depth == nullptr)
        return;
    if (record.dsv.sampleCount != 1)
    {
        RTSKY_LOG_ONCE(log::Level::Error, "The G-buffer depth is multisampled - not supported");
        return;
    }
    const DXGI_FORMAT srvFormat = DepthSrvFormat(record.dsv.resourceFormat);
    if (srvFormat == DXGI_FORMAT_UNKNOWN)
    {
        RTSKY_LOG_ONCE(log::Level::Error, "The G-buffer depth format (%d) cannot be sampled", static_cast<int>(record.dsv.resourceFormat));
        return;
    }

    // State of the depth plane at this point of the list
    GameResourceState depthState;
    {
        track::ObservedState o;
        if (track::FindObservedState(state, depth, record.barrierSeqAtLastDraw, &o))
        {
            if (!StateFromObserved(o, &depthState))
            {
                RTSKY_LOG_ONCE(log::Level::Warning, "Depth was transitioned to an unsupported (or split) state before the pass ended; skipping");
                return;
            }
        }
        else
        {
            if (record.dsvReadOnlyDepth)
            {
                // DEPTH_READ may be combined with shader-resource bits that we cannot see.
                RTSKY_LOG_ONCE(log::Level::Warning, "G-buffer depth bound read-only without an observed barrier; skipping");
                return;
            }
            const track::BarrierApi api = track::LastBarrierApi(depth);
            depthState.enhanced = api == track::BarrierApi::Enhanced ||
                                  (api == track::BarrierApi::Unknown && track::GameUsesEnhancedBarriers());
            depthState.usage = Usage::DepthWrite;
            depthState.legacyState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            depthState.layout = D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE;
        }
    }

    // Trace resolution = viewport of the G-buffer pass (dynamic resolution renders into a sub-rect)
    UINT vpX = 0, vpY = 0;
    UINT w = static_cast<UINT>(record.dsv.width);
    UINT h = record.dsv.height;
    if (record.viewportValid && record.viewport.Width >= 16.0f && record.viewport.Height >= 16.0f)
    {
        vpX = static_cast<UINT>(std::max(record.viewport.TopLeftX, 0.0f));
        vpY = static_cast<UINT>(std::max(record.viewport.TopLeftY, 0.0f));
        w = std::min(static_cast<UINT>(record.viewport.Width + 0.5f), static_cast<UINT>(record.dsv.width) - vpX);
        h = std::min(static_cast<UINT>(record.viewport.Height + 0.5f), record.dsv.height - vpY);
    }
    if (w < 16 || h < 16)
        return;

    AcquireSRWLockExclusive(&m_lock);

    // Screen resources are sized to the depth TEXTURE; the trace covers the viewport inside it. With
    // dynamic resolution the viewport changes often and only the history is reset (Composite sees the
    // trace size change), nothing is reallocated.
    const UINT texW = static_cast<UINT>(record.dsv.width);
    const UINT texH = record.dsv.height;
    if (!m_set || m_set->width != texW || m_set->height != texH)
    {
        const ULONGLONG now = GetTickCount64();
        if (now < m_setRetryAt)
        {
            m_lastSkip = "screen resource allocation failed recently";
            ReleaseSRWLockExclusive(&m_lock);
            return;
        }
        std::shared_ptr<ResourceSet> fresh = CreateSet(texW, texH);
        if (!fresh)
        {
            m_lastSkip = "screen resources not available yet";
            ReleaseSRWLockExclusive(&m_lock);
            return;
        }
        m_set = fresh;
        m_resetRequested.store(true);
    }

    // Join the open Prepare group (another parallel G-buffer list of this frame), or start one. A
    // group is open until a Composite consumes it; a different depth, size or set starts a new one.
    const bool join = m_pending.valid && m_groupSlotHandle && m_pending.prepareSerial != m_lastCompositePrepareSerial &&
                      m_groupDepth.Get() == depth && m_pending.set == m_set && m_pending.traceW == w && m_pending.traceH == h &&
                      m_pending.depthVpX == float(vpX) && m_pending.depthVpY == float(vpY) &&
                      GetTickCount64() - m_pending.recordedAt <= 250;
    if (!join && !BeginPrepareGroup(record, depth, srvFormat, vpX, vpY, w, h, cfg))
    {
        ReleaseSRWLockExclusive(&m_lock);
        return;
    }
    const PendingFrame f = m_pending;
    const uint32_t slot = m_groupSlot;
    const std::shared_ptr<void> slotHandle = m_groupSlotHandle;
    const uint32_t slotBase = SlotTableBase(slot);

    // --- record ---
    const bool alreadyReadable = !depthState.enhanced && depthState.usage == Usage::ComputeRead &&
                                 (depthState.legacyState & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) != 0;
    if (!alreadyReadable)
        TransitionGame(list, depth, depthState, Usage::ComputeRead, true);

    OwnTracker own;
    const ResourceSet& set = *m_set;
    own.Use(set.linearDepth[f.parity].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    own.Use(set.normal[f.parity].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    own.Flush(list);

    BeginPasses(list, slot, kTlasNull);
    Run(list, Pso::Prepare, slotBase, SetTableBase(set, ST_PrepareUav + f.parity) + kTableSrv, DivUp(w, RTSKY_GROUP_SIZE),
        DivUp(h, RTSKY_GROUP_SIZE), 1);

    own.Rest(list);
    if (!alreadyReadable)
        RestoreGame(list, depth, Usage::ComputeRead, depthState, true);
    track::RestoreState(list, state);

    Lifetime().AttachBusy(state, slotHandle);
    Lifetime().Attach(state, std::static_pointer_cast<void>(f.set));
    Lifetime().Attach(state, m_groupDepth);
    state.preparedSerial = f.prepareSerial;
    ++m_prepares;
    if (m_prepares.load() == 1)
        LOG_INFO("First Prepare injection recorded (%ux%u, depth format %d, %s barriers)", w, h,
                 static_cast<int>(record.dsv.resourceFormat), depthState.enhanced ? "enhanced" : "legacy");
    ReleaseSRWLockExclusive(&m_lock);
}

bool RendererImpl::BeginPrepareGroup(const BindingRecord& record, ID3D12Resource* depth, DXGI_FORMAT srvFormat, UINT vpX, UINT vpY,
                                     UINT w, UINT h, const Config& cfg)
{
    // Whatever happens below, the previous frame's Prepare must not be paired with this frame's
    // Composite (its depth and camera are a frame old).
    m_pending.valid = false;
    m_groupSlotHandle.reset();
    m_groupDepth.Reset();

    // Camera for this frame + calibration hypotheses
    // Pinned values restrict the calibration's search to their row / column (with the same
    // hysteresis as the automatic choice).
    m_calibration.SetPinned(cfg.cameraLatency, cfg.tlasSpace == TlasSpaceSetting::Auto ? -1 : static_cast<int>(cfg.tlasSpace));
    const int latency = m_calibration.Latency();
    const float aspect = float(w) / float(h);
    PendingFrame f;
    game::CameraSample sample;
    if (!game::Game().GetCamera(static_cast<uint32_t>(latency), &sample))
    {
        m_lastSkip = "no camera data from ScriptHookV yet";
        return false;
    }
    f.camera = MakeCamera(sample, aspect, cfg.fovScale);
    f.latencyUsed = latency;
    for (int l = 0; l < Calibration::kLatencies; ++l)
    {
        game::CameraSample hs;
        if (game::Game().GetCamera(static_cast<uint32_t>(l), &hs))
            f.hypotheses[l] = MakeCamera(hs, aspect, cfg.fovScale);
    }
    if (f.hypotheses[0].valid && f.hypotheses[3].valid)
    {
        const float moved = Length(f.hypotheses[0].position - f.hypotheses[3].position);
        const float turned = AngleBetweenDeg(f.hypotheses[0].forward, f.hypotheses[3].forward);
        f.informative = moved > 0.5f || turned > 1.0f;
    }

    // Depth encoding
    if (cfg.depthMode != DepthModeSetting::Auto)
    {
        f.depthMode = static_cast<uint32_t>(cfg.depthMode);
    }
    else
    {
        const track::InjectionRules rules = track::Analyzer().Rules();
        f.depthMode = (rules.depthClearKnown && rules.depthClearValue > 0.5f) ? RTSKY_DEPTH_STANDARD : RTSKY_DEPTH_REVERSED_FINITE;
    }

    f.set = m_set;
    f.prepareSerial = ++m_prepareSerial;
    f.parity = static_cast<uint32_t>(f.prepareSerial & 1);
    f.traceW = w;
    f.traceH = h;
    f.depthVpX = float(vpX);
    f.depthVpY = float(vpY);
    f.depthTexW = record.dsv.width;
    f.depthTexH = record.dsv.height;
    f.recordedAt = GetTickCount64();
    f.valid = true;

    uint32_t slot = 0;
    std::shared_ptr<void> slotHandle = ClaimSlot(&slot, 0, false, false);
    if (!slotHandle)
    {
        m_lastSkip = "all injection slots in flight";
        return false;
    }

    gpu::FrameConstants* fc = reinterpret_cast<gpu::FrameConstants*>(m_constantsMapped + SIZE_T(slot) * kConstantSlotSize);
    gpu::FrameConstants local;
    FillConstants(local, f, nullptr, cfg, false, 0, f.camera.position, 0, 0, w, h, false);
    std::memcpy(fc, &local, sizeof(local));

    // Game depth SRV in this slot's table (shared by every Prepare of the group: same depth)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
        d.Format = srvFormat;
        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Texture2D.MipLevels = 1;
        d.Texture2D.PlaneSlice = 0;
        m_device->CreateShaderResourceView(depth, &d, Cpu(SlotTableBase(slot)));
    }

    m_pending = f;
    m_groupSlot = slot;
    m_groupSlotHandle = std::move(slotHandle);
    m_groupDepth = depth;
    return true;
}

// -------------------------------------------------------------------------------------------------
// Injection 2: Composite (end of the HDR lighting pass)
// -------------------------------------------------------------------------------------------------
void RendererImpl::Composite(ID3D12GraphicsCommandList* list, ListState& state, const BindingRecord& record, const Config& cfg)
{
    ID3D12Resource* target = record.rtv[0].resource;
    if (target == nullptr)
        return;

    AcquireSRWLockExclusive(&m_lock);
    auto skip = [&](const char* reason) {
        m_lastSkip = reason;
        ReleaseSRWLockExclusive(&m_lock);
    };

    if (!m_pending.valid || !m_pending.set || m_pending.prepareSerial == m_lastCompositePrepareSerial ||
        GetTickCount64() - m_pending.recordedAt > 250)
        return skip("no Prepare for this frame");
    if (track::Tlas().OpacityMicromapsSeen() && !(cfg.tracePath == TracePath::Pipeline && m_rtPipeline.AllowsOpacityMicromaps()))
        return skip("the game uses opacity micromaps; only the DXR pipeline path on tier 1.2 can trace them");

    // Scene TLAS: a clone recorded earlier in this very list is ordered by the list itself; otherwise
    // the newest clone whose producing list was already submitted (the ExecuteCommandLists hook adds
    // a queue wait when that was on another queue).
    track::TlasInfo tlas;
    const bool tlasFromThisList = state.tlasProducedValid;
    if (tlasFromThisList)
        tlas = state.tlasProduced;
    else if (!track::Tlas().GetSceneTlas(&tlas))
        return skip("no scene TLAS captured (enable the game's ray tracing)");
    // Never trace a TLAS that is not from (about) this frame: the BLASes it references may be gone.
    if (tlas.buildSerial == m_lastTlasSerial)
    {
        if (++m_tlasReuse > 1)
            return skip("the scene TLAS was not rebuilt");
    }
    else
    {
        m_tlasReuse = 0;
    }

    const bool usePipeline = cfg.tracePath == TracePath::Pipeline && m_rtPipeline.IsValid();
    const PendingFrame f = m_pending;
    const ResourceSet& set = *f.set;

    // Calibration gate: a wrong camera / TLAS space would produce garbage occlusion.
    const bool calibrating = cfg.calibrationProbe && (cfg.cameraLatency < 0 || cfg.tlasSpace == TlasSpaceSetting::Auto);
    int tlasSpace = m_calibration.TlasSpace(); // the pinned value when TlasSpace is set in the INI

    // Composite target
    const D3D12_RESOURCE_DESC targetDesc = ResourceDesc(target);
    if (targetDesc.SampleDesc.Count != 1 || targetDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
        return skip("unsupported HDR target");
    // Only the bound subresource's state is known; the other mips / slices are in whatever state the
    // game left them, so whole-resource transitions and copies would be wrong.
    if (targetDesc.MipLevels != 1 || targetDesc.DepthOrArraySize != 1)
        return skip("the HDR target has several subresources");
    const DXGI_FORMAT uavFormat = record.rtv[0].viewFormat;
    if (!FormatSupportsTypedUav(uavFormat))
        return skip("the HDR target format does not support typed UAV loads");
    const bool inPlace = (targetDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;
    std::shared_ptr<SceneCopy> sceneCopy;
    if (!inPlace && !EnsureCopy(m_sceneCopy, m_sceneCopyRetryAt, targetDesc, L"RTSky scene copy", &sceneCopy))
        return skip("scene copy unavailable");

    GameResourceState targetState;
    {
        track::ObservedState o;
        if (track::FindObservedState(state, target, record.barrierSeqAtLastDraw, &o))
        {
            // The game already moved the target on (e.g. to a shader-read state for the next pass).
            if (!StateFromObserved(o, &targetState))
                return skip("the HDR target was transitioned to an unsupported (or split) state after its last draw");
        }
        else
        {
            const track::BarrierApi api = track::LastBarrierApi(target);
            targetState.enhanced = api == track::BarrierApi::Enhanced || (api == track::BarrierApi::Unknown && track::GameUsesEnhancedBarriers());
            targetState.usage = Usage::RenderTarget;
            targetState.legacyState = D3D12_RESOURCE_STATE_RENDER_TARGET;
            targetState.layout = D3D12_BARRIER_LAYOUT_RENDER_TARGET;
        }
    }

    UINT tx = 0, ty = 0, tw = static_cast<UINT>(targetDesc.Width), th = targetDesc.Height;
    if (record.viewportValid && record.viewport.Width >= 16.0f && record.viewport.Height >= 16.0f)
    {
        tx = static_cast<UINT>(std::max(record.viewport.TopLeftX, 0.0f));
        ty = static_cast<UINT>(std::max(record.viewport.TopLeftY, 0.0f));
        tw = std::min(static_cast<UINT>(record.viewport.Width + 0.5f), static_cast<UINT>(targetDesc.Width) - tx);
        th = std::min(static_cast<UINT>(record.viewport.Height + 0.5f), targetDesc.Height - ty);
    }

    // History validity
    bool reset = m_resetRequested.exchange(false);
    // Debug views write other data through the same history (e.g. the TLAS view): switching views
    // starts the accumulation over.
    if (cfg.debugView != m_lastDebugView)
    {
        m_lastDebugView = cfg.debugView;
        reset = true;
    }
    if (m_lastCompositeSet != &set || f.prepareSerial != m_lastCompositePrepareSerial + 1 || !m_lastCompositeCamera.valid ||
        f.traceW != m_lastCompositeTraceW || f.traceH != m_lastCompositeTraceH)
        reset = true;
    if (m_lastCompositeCamera.valid)
    {
        const float moved = Length(f.camera.position - m_lastCompositeCamera.position);
        const float turned = AngleBetweenDeg(f.camera.forward, m_lastCompositeCamera.forward);
        if (moved > 10.0f || turned > 30.0f)
            reset = true;
    }

    ++m_frameIndex;
    // The probe traces with an inline RayQuery, which cannot opt into opacity micromaps (that needs
    // SM 6.9); traversing OMM BLASes without opting in is undefined behaviour. Calibration then needs
    // explicit Latency / TlasSpace values (see docs/CALIBRATION.md).
    const bool ommBlocksProbe = track::Tlas().OpacityMicromapsSeen();
    const bool runProbe = calibrating && !ommBlocksProbe;
    if (calibrating && ommBlocksProbe)
        RTSKY_LOG_ONCE(log::Level::Warning, "The game uses opacity micromaps: the calibration probe is disabled. "
                       "Set [Camera] Latency and TlasSpace explicitly to enable relighting.");
    uint32_t slot = 0;
    std::shared_ptr<void> slotHandle = ClaimSlot(&slot, m_frameIndex, runProbe, f.informative);
    if (!slotHandle)
        return skip("all injection slots in flight");

    gpu::FrameConstants local;
    const bool srgbTarget = IsUnormColor(uavFormat);
    FillConstants(local, f, reset ? nullptr : &m_lastCompositeCamera, cfg, reset, tlasSpace, f.camera.position, tx, ty, tw, th, srgbTarget);
    std::memcpy(m_constantsMapped + SIZE_T(slot) * kConstantSlotSize, &local, sizeof(local));

    // UAV of the composite destination (slot table, UAV half)
    const uint32_t slotBase = SlotTableBase(slot);
    UavTex(slotBase + kTableSrv, inPlace ? target : sceneCopy->resource.Get(), uavFormat);
    UavTex(slotBase + kTableSrv + 1, set.debugView.Get(), kFmtColor);

    // --- record ---
    ComPtr<ID3D12GraphicsCommandList4> list4;
    if (usePipeline && FAILED(list->QueryInterface(IID_PPV_ARGS(&list4))))
        return skip("command list does not support DXR");

    OwnTracker own;
    const uint32_t p = f.parity;
    // Provisional TLAS; OnSubmit may still replace it with a newer clone ordered before this list.
    TlasSrv(kTlasBase + slot, tlas.address);
    BeginPasses(list, slot, kTlasBase + slot);

    // Atmosphere. The transmittance / multi-scattering LUTs are tiny (16K + 1K threads) and are
    // rebuilt every frame: a "built once" flag set at record time would be wrong if that list were
    // never submitted, and they follow the INI parameters without any invalidation logic.
    {
        own.Use(m_transmittance.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        own.Flush(list);
        Run(list, Pso::Transmittance, kGlobalBase + GT_Transmittance * kTableSize, kGlobalBase + GT_Transmittance * kTableSize + kTableSrv,
            DivUp(RTSKY_TRANSMITTANCE_W, 8), DivUp(RTSKY_TRANSMITTANCE_H, 8), 1);
        own.Use(m_transmittance.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        own.Use(m_multiScatter.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        own.Flush(list);
        Run(list, Pso::MultiScatter, kGlobalBase + GT_MultiScatter * kTableSize, kGlobalBase + GT_MultiScatter * kTableSize + kTableSrv,
            RTSKY_MULTISCATTER_SIZE, RTSKY_MULTISCATTER_SIZE, 1);
        own.Use(m_multiScatter.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    own.Use(m_skyView.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    own.Flush(list);
    Run(list, Pso::SkyView, kGlobalBase + GT_SkyView * kTableSize, kGlobalBase + GT_SkyView * kTableSize + kTableSrv,
        DivUp(RTSKY_SKYVIEW_W, 8), DivUp(RTSKY_SKYVIEW_H, 8), 1);
    own.Use(m_skyView.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    own.Use(m_skyData.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    own.Flush(list);
    Run(list, Pso::SkyProject, kGlobalBase + GT_SkyProject * kTableSize, kGlobalBase + GT_SkyProject * kTableSize + kTableSrv, 1, 1, 1);
    own.Use(m_skyData.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // Trace
    own.Use(set.traceS.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    own.Use(set.traceU.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    own.Flush(list);
    const uint32_t traceTable = SetTableBase(set, ST_Trace + p);
    list->SetComputeRootDescriptorTable(3, Gpu(traceTable));
    list->SetComputeRootDescriptorTable(4, Gpu(traceTable + kTableSrv));
    if (usePipeline)
        m_rtPipeline.Dispatch(list4.Get(), f.traceW, f.traceH);
    else
        Run(list, Pso::TraceInline, traceTable, traceTable + kTableSrv, DivUp(f.traceW, RTSKY_GROUP_SIZE), DivUp(f.traceH, RTSKY_GROUP_SIZE), 1);
    own.Use(set.traceS.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    own.Use(set.traceU.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // Temporal
    own.Use(set.histS[p].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    own.Use(set.histU[p].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    own.Use(set.histMeta[p].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    own.Flush(list);
    const uint32_t temporalTable = SetTableBase(set, ST_Temporal + p);
    Run(list, Pso::Temporal, temporalTable, temporalTable + kTableSrv, DivUp(f.traceW, RTSKY_GROUP_SIZE), DivUp(f.traceH, RTSKY_GROUP_SIZE), 1);
    own.Use(set.histS[p].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    own.Use(set.histU[p].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    own.Use(set.histMeta[p].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // Spatial filter
    const int iterations = (cfg.debugView == RTSKY_VIEW_TLAS || !cfg.denoiser) ? 0 : cfg.denoiseIterations;
    uint32_t compositeTable = ST_CompHist;
    for (int i = 0; i < iterations; ++i)
    {
        uint32_t table;
        int out;
        if (i == 0)
        {
            table = ST_AtrousHist;
            out = 0;
        }
        else if (i & 1)
        {
            table = ST_AtrousF0toF1;
            out = 1;
        }
        else
        {
            table = ST_AtrousF1toF0;
            out = 0;
        }
        own.Use(set.filtS[out].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        own.Use(set.filtU[out].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        own.Flush(list);
        gpu::PassConstants pc = {};
        pc.args0 = { static_cast<uint32_t>(i), 1u << i, 0, 0 };
        const uint32_t base = SetTableBase(set, table + p);
        Run(list, Pso::ATrous, base, base + kTableSrv, DivUp(f.traceW, RTSKY_GROUP_SIZE), DivUp(f.traceH, RTSKY_GROUP_SIZE), 1, &pc);
        own.Use(set.filtS[out].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        own.Use(set.filtU[out].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        compositeTable = out == 0 ? ST_CompF0 : ST_CompF1;
    }

    // Calibration probe
    if (runProbe)
    {
        own.Use(m_probeBuffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        own.Flush(list);
        const uint32_t probeTable = SetTableBase(set, ST_Probe + p);
        Run(list, Pso::Probe, probeTable, probeTable + kTableSrv, 1, 1, 1);
        own.Use(m_probeBuffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
        own.Flush(list);
        list->CopyBufferRegion(m_readback.Get(), UINT64(slot) * kReadbackSlotSize, m_probeBuffer.Get(), 0, kProbeUints * sizeof(uint32_t));
    }

    // Until the probe has confirmed the camera / TLAS-space hypothesis, the occlusion could be
    // garbage: keep tracing (the probe needs it) but leave the game's image untouched.
    // Gate on the hypothesis actually rendered with (a pinned value may differ from the probe's pick).
    const bool calibrated = !calibrating || cfg.forceRelight || (m_calibration.HasData(f.latencyUsed, tlasSpace) &&
                                             m_calibration.Confidence(f.latencyUsed, tlasSpace) >= cfg.minCalibrationScore);
    if (!calibrated && cfg.debugView == RTSKY_VIEW_NONE)
    {
        own.Rest(list);
        track::RestoreState(list, state);
        Lifetime().AttachBusy(state, slotHandle);
        Lifetime().Attach(state, std::static_pointer_cast<void>(f.set));
        Lifetime().AttachBusy(state, tlas.cloneHolder);
        Lifetime().Attach(state, tlas.cloneResource);
        if (!tlasFromThisList)
        {
            state.tlasConsumed = tlas;
            state.tlasConsumedValid = true;
        }
        state.compositeConsumedSerial = f.prepareSerial;
        if (!tlasFromThisList)
            state.tlasDescriptor = static_cast<int32_t>(kTlasBase + slot);
        m_lastTlasSerial = tlas.buildSerial;
        m_lastCompositePrepareSerial = f.prepareSerial;
        m_lastCompositeCamera = f.camera;
        m_lastCompositeSet = &set;
        m_lastCompositeTraceW = f.traceW;
        m_lastCompositeTraceH = f.traceH;
        RTSKY_LOG_ONCE(log::Level::Info, "Calibrating camera / TLAS space before relighting (see the status line in RTSky.log)");
        m_lastHeldTick.store(GetTickCount64(), std::memory_order_relaxed);
        m_lastSkip = "calibrating";
        ReleaseSRWLockExclusive(&m_lock);
        return;
    }

    // Composite into the game's HDR target (and the current debug view into DebugView)
    gpu::PassConstants none = {};
    const uint32_t compBase = SetTableBase(set, compositeTable + p);
    own.Use(set.debugView.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (inPlace)
    {
        own.Flush(list);
        TransitionGame(list, target, targetState, Usage::ComputeWrite, false);
        Run(list, Pso::Composite, compBase, slotBase + kTableSrv, DivUp(tw, RTSKY_GROUP_SIZE), DivUp(th, RTSKY_GROUP_SIZE), 1, &none);
        RestoreGame(list, target, Usage::ComputeWrite, targetState, false);
    }
    else
    {
        ID3D12Resource* copy = sceneCopy->resource.Get();
        own.Use(copy, D3D12_RESOURCE_STATE_COPY_DEST);
        own.Flush(list);
        TransitionGame(list, target, targetState, Usage::CopySource, false);
        list->CopyResource(copy, target);
        own.Use(copy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        own.Flush(list);
        Run(list, Pso::Composite, compBase, slotBase + kTableSrv, DivUp(tw, RTSKY_GROUP_SIZE), DivUp(th, RTSKY_GROUP_SIZE), 1, &none);
        own.Use(copy, D3D12_RESOURCE_STATE_COPY_SOURCE);
        own.Flush(list);
        GameResourceState asSource;
        asSource.enhanced = targetState.enhanced;
        asSource.usage = Usage::CopySource;
        asSource.legacyState = D3D12_RESOURCE_STATE_COPY_SOURCE;
        asSource.layout = D3D12_BARRIER_LAYOUT_COPY_SOURCE;
        TransitionGame(list, target, asSource, Usage::CopyDest, false);
        list->CopyResource(target, copy);
        RestoreGame(list, target, Usage::CopyDest, targetState, false);
    }

    own.Rest(list);
    track::RestoreState(list, state);

    // Keep everything the GPU will touch alive until this list has executed.
    Lifetime().AttachBusy(state, slotHandle);
    Lifetime().Attach(state, std::static_pointer_cast<void>(f.set));
    Lifetime().AttachBusy(state, tlas.cloneHolder);
    Lifetime().Attach(state, tlas.cloneResource);
    if (!tlasFromThisList)
    {
        state.tlasConsumed = tlas;
        state.tlasConsumedValid = true;
    }
    state.compositeConsumedSerial = f.prepareSerial;
    if (!tlasFromThisList)
        state.tlasDescriptor = static_cast<int32_t>(kTlasBase + slot);
    if (sceneCopy)
        Lifetime().Attach(state, std::static_pointer_cast<void>(sceneCopy));

    m_lastTlasSerial = tlas.buildSerial;
    m_lastCompositePrepareSerial = f.prepareSerial;
    m_lastCompositeCamera = f.camera;
    m_lastCompositeSet = &set;
    m_lastCompositeTraceW = f.traceW;
    m_lastCompositeTraceH = f.traceH;
    ++m_composites;
    m_lastRelitTick.store(GetTickCount64(), std::memory_order_relaxed);
    if (m_composites.load() == 1)
        LOG_INFO("First Composite injection recorded (%s, %s, target format %d, %ux%u)", usePipeline ? "DXR pipeline" : "inline RayQuery",
                 inPlace ? "in place" : "via copy", static_cast<int>(uavFormat), tw, th);
    m_lastSkip.clear();
    ReleaseSRWLockExclusive(&m_lock);
}

// -------------------------------------------------------------------------------------------------
// Injection 3: DebugBlit (end of PS_LensDistortion, the final image before the UI)
// -------------------------------------------------------------------------------------------------
namespace {
// Format a compute shader can store into for a render target: the view's format without sRGB (UAVs
// cannot be sRGB), as long as the resource is typeless or already has that format.
DXGI_FORMAT StoreFormat(DXGI_FORMAT resourceFormat, DXGI_FORMAT viewFormat)
{
    DXGI_FORMAT f = viewFormat;
    if (f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
        f = DXGI_FORMAT_B8G8R8A8_UNORM;
    else if (f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
        f = DXGI_FORMAT_R8G8B8A8_UNORM;
    const bool typeless = resourceFormat == DXGI_FORMAT_B8G8R8A8_TYPELESS || resourceFormat == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
                          resourceFormat == DXGI_FORMAT_R10G10B10A2_TYPELESS || resourceFormat == DXGI_FORMAT_R16G16B16A16_TYPELESS;
    return typeless || resourceFormat == f ? f : DXGI_FORMAT_UNKNOWN;
}
} // namespace

void RendererImpl::DebugBlit(ID3D12GraphicsCommandList* list, ListState& state, const BindingRecord& record, const Config& cfg)
{
    if (cfg.debugView == RTSKY_VIEW_NONE)
        return;
    ID3D12Resource* target = record.rtv[0].resource;
    if (record.rtvCount < 1 || target == nullptr)
        return;

    AcquireSRWLockExclusive(&m_lock);
    auto skip = [&](const char* reason) {
        m_blitSkip = reason;
        ReleaseSRWLockExclusive(&m_lock);
    };
    // Only once the Composite has written a debug view recently (it draws into DebugView).
    if (!m_set || m_lastCompositeTraceW == 0 || GetTickCount64() - m_lastRelitTick.load() > 1000)
        return skip("no debug view written recently");
    const std::shared_ptr<ResourceSet> set = m_set;

    const D3D12_RESOURCE_DESC desc = ResourceDesc(target);
    if (desc.SampleDesc.Count != 1 || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
        return skip("unsupported final image");
    if (desc.MipLevels != 1 || desc.DepthOrArraySize != 1)
        return skip("the final image has several subresources");
    const DXGI_FORMAT uavFormat = StoreFormat(desc.Format, record.rtv[0].viewFormat);
    if (uavFormat == DXGI_FORMAT_UNKNOWN || !FormatSupportsTypedStore(uavFormat))
        return skip("the final image's format cannot be written by a compute shader");
    const bool inPlace = (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;
    std::shared_ptr<SceneCopy> copy;
    if (!inPlace && !EnsureCopy(m_blitCopy, m_blitCopyRetryAt, desc, L"RTSky debug blit copy", &copy))
        return skip("debug blit copy unavailable");

    // State of the final image at the end of its binding (as for the Composite's target).
    GameResourceState targetState;
    {
        track::ObservedState o;
        if (track::FindObservedState(state, target, record.barrierSeqAtLastDraw, &o))
        {
            if (!StateFromObserved(o, &targetState))
                return skip("the final image was transitioned to an unsupported (or split) state after its last draw");
        }
        else
        {
            const track::BarrierApi api = track::LastBarrierApi(target);
            targetState.enhanced = api == track::BarrierApi::Enhanced || (api == track::BarrierApi::Unknown && track::GameUsesEnhancedBarriers());
            targetState.usage = Usage::RenderTarget;
            targetState.legacyState = D3D12_RESOURCE_STATE_RENDER_TARGET;
            targetState.layout = D3D12_BARRIER_LAYOUT_RENDER_TARGET;
        }
    }

    UINT tx = 0, ty = 0, tw = static_cast<UINT>(desc.Width), th = desc.Height;
    if (record.viewportValid && record.viewport.Width >= 16.0f && record.viewport.Height >= 16.0f)
    {
        tx = static_cast<UINT>(std::max(record.viewport.TopLeftX, 0.0f));
        ty = static_cast<UINT>(std::max(record.viewport.TopLeftY, 0.0f));
        tw = std::min(static_cast<UINT>(record.viewport.Width + 0.5f), static_cast<UINT>(desc.Width) - tx);
        th = std::min(static_cast<UINT>(record.viewport.Height + 0.5f), desc.Height - ty);
    }

    uint32_t slot = 0;
    std::shared_ptr<void> slotHandle = ClaimSlot(&slot, 0, false, false);
    if (!slotHandle)
        return skip("all injection slots in flight");
    // The blit reads only its pass constants; the frame constants are bound but unused.
    std::memset(m_constantsMapped + SIZE_T(slot) * kConstantSlotSize, 0, sizeof(gpu::FrameConstants));
    const uint32_t slotBase = SlotTableBase(slot);
    SrvTex(slotBase + 0, set->debugView.Get(), kFmtColor);
    UavTex(slotBase + kTableSrv, inPlace ? target : copy->resource.Get(), uavFormat);
    gpu::PassConstants pc = {};
    pc.args0 = { tx, ty, tw, th };
    pc.args1 = { m_lastCompositeTraceW, m_lastCompositeTraceH, 0, 0 };

    // --- record ---
    OwnTracker own;
    BeginPasses(list, slot, kTlasNull);
    if (inPlace)
    {
        TransitionGame(list, target, targetState, Usage::ComputeWrite, false);
        Run(list, Pso::DebugBlit, slotBase, slotBase + kTableSrv, DivUp(tw, RTSKY_GROUP_SIZE), DivUp(th, RTSKY_GROUP_SIZE), 1, &pc);
        RestoreGame(list, target, Usage::ComputeWrite, targetState, false);
    }
    else
    {
        // Every pixel of the viewport is written, so the copy needs no copy of the game's image first;
        // only the viewport goes back.
        ID3D12Resource* c = copy->resource.Get();
        own.Use(c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        own.Flush(list);
        Run(list, Pso::DebugBlit, slotBase, slotBase + kTableSrv, DivUp(tw, RTSKY_GROUP_SIZE), DivUp(th, RTSKY_GROUP_SIZE), 1, &pc);
        own.Use(c, D3D12_RESOURCE_STATE_COPY_SOURCE);
        own.Flush(list);
        TransitionGame(list, target, targetState, Usage::CopyDest, false);
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = target;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION src = dst;
        src.pResource = c;
        const D3D12_BOX box = { tx, ty, 0, tx + tw, ty + th, 1 };
        list->CopyTextureRegion(&dst, tx, ty, 0, &src, &box);
        RestoreGame(list, target, Usage::CopyDest, targetState, false);
    }
    own.Rest(list);
    track::RestoreState(list, state);

    Lifetime().AttachBusy(state, slotHandle);
    Lifetime().Attach(state, std::static_pointer_cast<void>(set));
    if (copy)
        Lifetime().Attach(state, std::static_pointer_cast<void>(copy));
    if (m_lastBlitTick.exchange(GetTickCount64()) == 0)
        LOG_INFO("First debug blit recorded (%s, format %d, %ux%u, %s)", track::PassName(record.firstPassId), static_cast<int>(uavFormat), tw,
                 th, inPlace ? "in place" : "via copy");
    m_blitSkip.clear();
    ReleaseSRWLockExclusive(&m_lock);
}

void RendererImpl::OnSubmit(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    // Late TLAS binding. At recording time the Composite could only use a clone whose producing list
    // was already submitted (or completed); now, at submission, the GPU order is known: a clone
    // written by an earlier list of this same call, or one submitted earlier on this queue, runs
    // before this list. Use the newest such clone (normally this frame's) instead of the provisional
    // one. One-shot per recording: a re-executed list keeps its descriptor (a previous execution
    // may still be reading it).
    if (m_init.load(std::memory_order_acquire) == InitState::Ready)
    {
        for (UINT i = 0; i < count; ++i)
        {
            ListState* s = track::FindListState(lists[i]);
            if (s == nullptr || s->tlasDescriptor < 0)
                continue;
            track::TlasInfo best = s->tlasConsumed;
            for (UINT j = 0; j < i; ++j)
            {
                const ListState* p = track::FindListState(lists[j]);
                if (p != nullptr && p->tlasProducedValid && p->tlasProduced.buildSerial > best.buildSerial)
                    best = p->tlasProduced;
            }
            track::TlasInfo published;
            if (track::Tlas().GetSceneTlas(&published, queue) && published.buildSerial > best.buildSerial)
                best = published;
            if (best.buildSerial != s->tlasConsumed.buildSerial && best.address != 0)
            {
                TlasSrv(static_cast<uint32_t>(s->tlasDescriptor), best.address);
                // Moved into this submission's lifetime batch by GpuLifetime::OnExecuted (called next).
                Lifetime().AttachBusy(*s, best.cloneHolder);
                Lifetime().Attach(*s, best.cloneResource);
                s->tlasConsumed = best;
                m_lateTlas.fetch_add(1, std::memory_order_relaxed);
            }
            s->tlasDescriptor = -1;
        }
    }

    {
    // Prepare and Composite are paired at recording time. With parallel recording the lighting list
    // can be recorded before this frame's G-buffer list, and the Composite then pairs with the
    // previous frame's Prepare (one frame of lag). Detect it in GPU order and report it.
    for (UINT i = 0; i < count; ++i)
    {
        const ListState* s = track::FindListState(lists[i]);
        if (s == nullptr)
            continue;
        if (s->preparedSerial != 0)
            m_lastExecutedPrepare.store(s->preparedSerial, std::memory_order_relaxed);
        if (s->compositeConsumedSerial != 0 && s->compositeConsumedSerial != m_lastExecutedPrepare.load(std::memory_order_relaxed))
        {
            if (m_pairingLag.fetch_add(1) == 0)
                LOG_WARN("The HDR lighting list was recorded before this frame's G-buffer list: relighting lags one frame "
                         "(see docs/CALIBRATION.md, 'Pairing')");
        }
    }
    }
}

std::string RendererImpl::Status()
{
    const char* init = "not started";
    switch (m_init.load())
    {
    case InitState::Running: init = "initialising"; break;
    case InitState::Ready: init = "ready"; break;
    case InitState::Failed: init = "FAILED"; break;
    default: break;
    }
    AcquireSRWLockShared(&m_lock);
    std::string skip = m_lastSkip;
    ReleaseSRWLockShared(&m_lock);
    char buf[512];
    snprintf(buf, sizeof(buf), "renderer %s, %llu prepares, %llu composites (%llu paired late, %llu TLAS bound at submit), %llu pipelines noted, calibration: %s%s%s",
             init, static_cast<unsigned long long>(m_prepares.load()), static_cast<unsigned long long>(m_composites.load()),
             static_cast<unsigned long long>(m_pairingLag.load()), static_cast<unsigned long long>(m_lateTlas.load()),
             static_cast<unsigned long long>(track::NotedPipelines()),
             m_calibration.Describe().c_str(), skip.empty() ? "" : ", last skip: ", skip.c_str());
    return buf;
}

const char* ViewName(int view)
{
    switch (view)
    {
    case RTSKY_VIEW_NONE: return "off";
    case RTSKY_VIEW_SKY_RATIO: return "1 sky visibility";
    case RTSKY_VIEW_SUN_VIS: return "2 sun visibility";
    case RTSKY_VIEW_NORMALS: return "3 normals";
    case RTSKY_VIEW_DEPTH: return "4 depth";
    case RTSKY_VIEW_SKY_S: return "5 sky irradiance";
    case RTSKY_VIEW_TLAS: return "6 TLAS alignment";
    case RTSKY_VIEW_RATIO: return "7 lighting multiplier";
    case RTSKY_VIEW_HISTORY: return "8 temporal history";
    default: return "?";
    }
}

std::vector<std::string> RendererImpl::OverlayLines(const Config& cfg)
{
    std::vector<std::string> lines;
    char buf[256];
    const ULONGLONG now = GetTickCount64();
    const InitState init = m_init.load();
    const track::InjectionRules rules = track::Analyzer().Rules();
    const std::string analyzer = track::Analyzer().Status();
    AcquireSRWLockShared(&m_lock);
    const std::string skip = m_lastSkip;
    ReleaseSRWLockShared(&m_lock);
    const bool relitRecently = now - m_lastRelitTick.load() < 1500;
    const bool heldRecently = now - m_lastHeldTick.load() < 1500;
    const bool calibrating = cfg.calibrationProbe && (cfg.cameraLatency < 0 || cfg.tlasSpace == TlasSpaceSetting::Auto);
    const float confidence = m_calibration.Confidence();

    // 1. Verdict: the first thing that stops RTSky, in pipeline order.
    std::string verdict;
    if (!cfg.enabled)
        verdict = "RTSky OFF";
    else if (!hooks::Installed())
        verdict = "RTSky: D3D12 hooks not installed (see RTSky.log)";
    else if (!rules.armed)
        verdict = "RTSky: looking for the G-buffer / lighting passes";
    else if (init == InitState::Failed)
        verdict = "RTSky: renderer failed to start (see RTSky.log)";
    else if (init != InitState::Ready)
        verdict = "RTSky: starting the renderer";
    else if (relitRecently)
        verdict = cfg.debugView != RTSKY_VIEW_NONE ? "RTSky ACTIVE - showing a debug view" : "RTSky ACTIVE - relighting";
    else if (heldRecently)
        verdict = "RTSky: tracing, relighting HELD until calibration passes";
    else
        verdict = "RTSky: not relighting - " + (skip.empty() ? std::string("no composite recorded yet") : skip);
    lines.push_back(verdict);

    // 2. Details
    const char* initName = init == InitState::Ready ? "ready" : init == InitState::Running ? "starting" : init == InitState::Failed ? "FAILED" : "idle";
    snprintf(buf, sizeof(buf), "%s | hooks %s | renderer %s | %s", RTSKY_VERSION, hooks::Installed() ? "OK" : "waiting", initName,
             m_rtPipeline.IsValid() && cfg.tracePath == TracePath::Pipeline ? "DXR pipeline" : "inline RayQuery");
    lines.push_back(buf);
    lines.push_back("Frame: " + analyzer);
    snprintf(buf, sizeof(buf), "TLAS: %llu builds seen%s | prepares %llu | composites %llu | late %llu",
             static_cast<unsigned long long>(track::Tlas().TopLevelBuilds()), track::Tlas().OpacityMicromapsSeen() ? " (OMM)" : "",
             static_cast<unsigned long long>(m_prepares.load()), static_cast<unsigned long long>(m_composites.load()),
             static_cast<unsigned long long>(m_pairingLag.load()));
    lines.push_back(buf);
    const int pixels = m_calibration.LastProbePixels();
    if (calibrating)
    {
        snprintf(buf, sizeof(buf), "Calibration: score %.2f (needs %.2f)%s%s | depth samples %d/64", confidence, cfg.minCalibrationScore,
                 cfg.forceRelight ? " FORCED" : "", confidence >= cfg.minCalibrationScore ? " OK" : "", pixels < 0 ? 0 : pixels);
        lines.push_back(buf);
        lines.push_back(m_calibration.Describe());
    }
    else
    {
        lines.push_back("Calibration: off (pinned Latency / TlasSpace)");
    }
    if (!skip.empty())
        lines.push_back("Last skip: " + skip);
    // Game states that fade the relighting out (InteriorStrength / CutsceneStrength / loading).
    const game::EnvironmentSample env = game::Game().GetEnvironment();
    if (env.valid && (env.loading || (env.interior && cfg.interiorStrength < 1.0f) || (env.cutscene && cfg.cutsceneStrength < 1.0f)))
    {
        snprintf(buf, sizeof(buf), "Faded out: %s%s%s", env.loading ? "loading screen " : "",
                 env.interior ? "interior (InteriorStrength) " : "", env.cutscene ? "cutscene (CutsceneStrength)" : "");
        lines.push_back(buf);
    }
    const char* foliage = cfg.foliageMode == FoliageMode::Opaque ? "opaque" : cfg.foliageMode == FoliageMode::Ignore ? "ignored" : "stochastic";
    snprintf(buf, sizeof(buf), "View %s | compare %s | strength %.2f | sun %s | foliage %s | near %s | denoise %s",
             ViewName(cfg.debugView), cfg.compareSplit ? "ON" : "off", cfg.strength, cfg.sunShadowRays ? "on" : "off", foliage,
             cfg.nearField ? "on" : "off", cfg.denoiser ? "on" : "off");
    lines.push_back(buf);
    if (cfg.debugView != RTSKY_VIEW_NONE)
    {
        AcquireSRWLockShared(&m_lock);
        const std::string blitSkip = m_blitSkip;
        ReleaseSRWLockShared(&m_lock);
        if (now - m_lastBlitTick.load() < 1000)
            lines.push_back("Debug view: drawn into the final image (" + cfg.debugBlitPass + ")");
        else
            lines.push_back("Debug view: drawn into the scene - " +
                            (blitSkip.empty() ? cfg.debugBlitPass + " not seen yet" : blitSkip));
    }
    return lines;
}

void RendererImpl::ScriptTick()
{
    const ULONGLONG now = GetTickCount64();
    if (now - m_lastStatusLog >= 10000)
    {
        m_lastStatusLog = now;
        // The same verdict the on-screen status shows, so a log alone tells which stage stops RTSky.
        const std::vector<std::string> lines = OverlayLines(ConfigSnapshot());
        LOG_INFO("%s", lines.empty() ? "" : lines[0].c_str());
        LOG_INFO("Status: %s | analyzer: %s", Status().c_str(), track::Analyzer().Status().c_str());
    }
}

} // namespace

// -------------------------------------------------------------------------------------------------
// Public entry points
// -------------------------------------------------------------------------------------------------
void OnBindingClosed(ID3D12GraphicsCommandList* list, track::ListState& state, const track::BindingRecord& record)
{
    if (!state.sawReset || state.stateUnknown)
        return; // root / heap state unknown (list first seen mid-recording, or a bundle was executed)
    const track::FrameAnalyzer& analyzer = track::Analyzer();
    // Every-binding mode: a list that re-binds the G-buffer (LOD objects) gets a Prepare after each
    // binding, so the last one on the GPU still sees all of the depth.
    const bool prepare = (!state.injectedPrepare || analyzer.PrepareEveryBinding()) && analyzer.MatchPrepare(state, record);
    const bool composite = !state.injectedComposite && analyzer.MatchComposite(state, record);
    const bool blit = !state.injectedDebugBlit && analyzer.MatchDebugBlit(state, record);
    if (!prepare && !composite && !blit)
        return;
    if (record.noInjectAfter)
    {
        // A suspending render pass (resumed by the next one) or PRESERVE_LOCAL ending accesses: no
        // other command may be recorded before the pass continues.
        RTSKY_LOG_ONCE(log::Level::Warning, "The matched pass suspends / preserves its render pass; cannot inject after it");
        return;
    }

    const Config cfg = ConfigSnapshot();
    if (!cfg.enabled)
        return;

    RendererImpl& r = Instance();
    if (!r.EnsureReady(list))
        return;

    // Copy: the record lives in the list's log, which RTSky's own (bypassed) calls never modify, but
    // a copy keeps this robust.
    const track::BindingRecord rec = record;
    hooks::HookBypass bypass;
    if (prepare)
    {
        state.injectedPrepare = true;
        r.Prepare(list, state, rec, cfg);
    }
    if (composite)
    {
        state.injectedComposite = true;
        r.Composite(list, state, rec, cfg);
    }
    if (blit)
    {
        state.injectedDebugBlit = true;
        r.DebugBlit(list, state, rec, cfg);
    }
}

void OnSubmit(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    Instance().OnSubmit(queue, count, lists);
}

void OnScriptTick()
{
    Instance().ScriptTick();
}

std::vector<std::string> OverlayLines(const Config& cfg)
{
    return Instance().OverlayLines(cfg);
}

void ResetCalibration()
{
    Instance().ResetCalibration();
}

const char* DebugViewName(int view)
{
    return ViewName(view);
}

std::string RendererStatus()
{
    return Instance().Status();
}

void ResetHistory()
{
    Instance().RequestReset();
}

} // namespace rtsky::render
