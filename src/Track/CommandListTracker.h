// RTSky - per command list recorded state
//
// Everything the game records on a command list that RTSky needs later:
//   * the full root / pipeline / descriptor-heap state, so that it can be restored exactly after
//     RTSky has recorded its own work in the middle of the game's list;
//   * render-target bindings and draw counts (the binding log), which the FrameAnalyzer uses to find
//     the G-buffer and HDR lighting passes, and which trigger the injections;
//   * barriers the game recorded on this list, to know the exact state of a game resource at the
//     injection point.
// A command list is recorded by one thread at a time (D3D12 rule), so a ListState needs no lock.
#pragma once

#include <d3d12.h>

#include "TlasTracker.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace rtsky::track {

struct BoundTarget
{
    ID3D12Resource* resource = nullptr;
    DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT resourceFormat = DXGI_FORMAT_UNKNOWN;
    UINT64 width = 0;
    UINT height = 0;
    UINT sampleCount = 1;
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
};

struct BindingRecord
{
    uint32_t rtvCount = 0;
    BoundTarget rtv[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT];
    bool hasDsv = false;
    BoundTarget dsv;
    bool dsvReadOnlyDepth = false;
    bool dsvReadOnlyStencil = false;
    D3D12_VIEWPORT viewport = {};
    bool viewportValid = false;
    uint32_t draws = 0;
    int32_t mrtOrdinal = -1;   // ordinal among this list's MRT bindings (>= 3 RTVs + DSV, draws > 0)
    int32_t hdrOrdinal = -1;   // ordinal among this list's float-RTV0 bindings with draws > 0
    uint32_t listSeq = 0;      // index of the binding within the list
    uint32_t barrierSeqAtLastDraw = 0;
    bool fromRenderPass = false;
    bool noInjectAfter = false; // suspending render pass / PRESERVE_LOCAL ending access: nothing may follow
    bool dsvCleared = false;
    float dsvClearDepth = 0.0f;
};

// Classification helpers shared by the tracker, the analyzer and the injector.
bool IsFloatHdrFormat(DXGI_FORMAT f);
bool IsMrtCandidate(const BindingRecord& r);
bool IsHdrCandidate(const BindingRecord& r);

// State of subresource 0 (mip 0, slice 0, plane 0) of a resource, as left by the last barrier the
// game recorded on this list that covered it. That is the only subresource RTSky ever transitions
// (the depth plane of the G-buffer depth, and single-subresource HDR targets).
struct ObservedState
{
    uint32_t stateOrLayout = 0; // D3D12_RESOURCE_STATES (legacy) or D3D12_BARRIER_LAYOUT (enhanced)
    bool enhanced = false;
    bool splitPending = false;  // a split barrier (BEGIN_ONLY / SyncAfter == SPLIT) has not ended yet
    D3D12_BARRIER_SYNC syncAfter = D3D12_BARRIER_SYNC_NONE;       // enhanced only
    D3D12_BARRIER_ACCESS accessAfter = D3D12_BARRIER_ACCESS_COMMON; // enhanced only
    uint32_t seq = 0;           // barrier sequence number within the list
};

struct BindPointState
{
    enum ArgType : uint8_t { ArgNone = 0, ArgTable, ArgCBV, ArgSRV, ArgUAV };
    struct Arg
    {
        ArgType type = ArgNone;
        uint64_t value = 0; // GPU descriptor handle or GPU virtual address
    };
    struct ConstEntry
    {
        uint8_t param;
        uint8_t offset;
        uint32_t value;
    };

    static constexpr uint32_t kMaxRootParams = 64;
    ID3D12RootSignature* rootSignature = nullptr;
    Arg args[kMaxRootParams];
    std::vector<ConstEntry> constants;

    void ClearArgs();
    void SetConstant(uint32_t param, uint32_t offset, uint32_t value);
};

struct ListState
{
    ID3D12GraphicsCommandList* list = nullptr;
    D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    uint64_t resetSerial = 0;
    // True once a Reset (or the creation of an open list) was observed. A list first seen in the
    // middle of a recording has an unknown root / heap state that RestoreState could not restore,
    // so RTSky never injects into it before its next Reset.
    bool sawReset = false;

    // Pipeline: the last of SetPipelineState / SetPipelineState1 wins.
    enum class PipelineKind : uint8_t { None, Pso, StateObject };
    PipelineKind pipelineKind = PipelineKind::None;
    ID3D12PipelineState* pso = nullptr;
    ID3D12StateObject* stateObject = nullptr;

    ID3D12DescriptorHeap* heaps[2] = {};
    uint32_t heapCount = 0;
    BindPointState compute;
    BindPointState graphics;

    // Current render-target binding
    bool bindingOpen = false;
    BindingRecord current;
    D3D12_VIEWPORT viewport = {};
    bool viewportValid = false;
    bool inRenderPass = false;
    uint32_t mrtCount = 0;
    uint32_t hdrCount = 0;
    uint32_t bindingSeq = 0;

    // Log of closed bindings (consumed by the analyzer at ExecuteCommandLists)
    std::vector<BindingRecord> log;

    // Barriers observed on this list, per resource
    std::unordered_map<ID3D12Resource*, ObservedState> observed;
    uint32_t barrierSeq = 0;

    // Scene TLAS clone recorded in this list; published to the tracker when the list is submitted.
    bool tlasProducedValid = false;
    TlasInfo tlasProduced;
    // Published clone that RTSky's trace in this list reads: when it was produced on another queue,
    // the ExecuteCommandLists hook makes the submitting queue wait for the producer's fence.
    bool tlasConsumedValid = false;
    TlasInfo tlasConsumed;

    // Statistics for the frame dump
    uint32_t tlasBuilds = 0;
    uint32_t blasBuilds = 0;
    uint32_t dispatchRays = 0;
    uint32_t draws = 0;

    // Injection bookkeeping (owned by the Renderer)
    bool injectedPrepare = false;
    bool injectedComposite = false;
    // Objects referenced by commands RTSky recorded into this list; kept alive until the GPU has
    // finished every execution of the list (see render::GpuLifetime).
    std::vector<std::shared_ptr<void>> attachments;

    void ResetForRecording(ID3D12PipelineState* initialPso);
};

// Returns the state for a list, creating it on first sight. Never returns nullptr.
ListState* GetListState(ID3D12GraphicsCommandList* list);
// Returns the state if the list is known, else nullptr (no creation).
ListState* FindListState(ID3D12CommandList* list);

// Recording helpers called by the hooks
void OnReset(ListState& s, ID3D12PipelineState* initialPso);
void OnSetRenderTargets(ListState& s, UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, BOOL singleRange,
                        const D3D12_CPU_DESCRIPTOR_HANDLE* dsv, UINT rtvDescriptorIncrement);
void OnBeginRenderPass(ListState& s, UINT count, const D3D12_RENDER_PASS_RENDER_TARGET_DESC* rts,
                       const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* ds, D3D12_RENDER_PASS_FLAGS flags);
// Closes the open binding and appends it to the log. Returns a pointer to the logged record if one
// was closed (valid until the next call that modifies the list state), else nullptr.
const BindingRecord* CloseBinding(ListState& s);
void OnDraw(ListState& s);
void OnViewports(ListState& s, UINT count, const D3D12_VIEWPORT* viewports);
void OnClearDepth(ListState& s, D3D12_CPU_DESCRIPTOR_HANDLE dsv, D3D12_CLEAR_FLAGS flags, float depth);
void OnResourceBarrier(ListState& s, UINT count, const D3D12_RESOURCE_BARRIER* barriers);
void OnEnhancedBarrier(ListState& s, UINT count, const D3D12_BARRIER_GROUP* groups);

// State of subresource 0 of `resource` if a barrier covering it was recorded on this list after
// barrier sequence `afterSeq` (e.g. after the last draw of a binding); false if none (caller infers).
bool FindObservedState(const ListState& s, ID3D12Resource* resource, uint32_t afterSeq, ObservedState* out);

// Global knowledge about which barrier API the game uses per resource
enum class BarrierApi : uint8_t { Unknown, Legacy, Enhanced };
BarrierApi LastBarrierApi(ID3D12Resource* resource);
bool GameUsesEnhancedBarriers();

// Replays the recorded root/pipeline/heap state onto the list (RTSky calls this after injecting).
void RestoreState(ID3D12GraphicsCommandList* list, const ListState& s);

} // namespace rtsky::track
