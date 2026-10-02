// RTSky - frame structure analysis
//
// Works on the binding logs of the command lists the game submits, in GPU execution order (the
// order of ExecuteCommandLists), never on recording order. From them it identifies:
//   * the G-buffer pass: the MRT signature (>= 3 render targets + depth) with the most draws; its
//     consecutive bindings (re-binds, split lists, suspended render passes) form one phase per frame
//     and the injection point is the phase's LAST binding;
//   * the HDR lighting pass: the n-th binding after the G-buffer phase whose first render target has
//     a float format and the G-buffer's dimensions.
// A rule is only armed when it matches exactly one binding per frame.
// It turns them into two injection rules that are matched cheaply at recording time:
//   Prepare   (end of the G-buffer pass: depth is known to be writable -> DEPTH_WRITE)
//   Composite (end of the HDR lighting pass: the target was just drawn to -> RENDER_TARGET)
// Rules match by signature (formats, dimensions, target count) plus the binding's ordinal within its
// command list, so ping-ponged resources and multi-threaded recording are handled.
#pragma once

#include "CommandListTracker.h"

#include <windows.h>

#include <atomic>
#include <string>
#include <vector>

namespace rtsky::track {

struct BindingSignature
{
    uint32_t rtvCount = 0;
    DXGI_FORMAT rtvFormats[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    bool hasDsv = false;
    DXGI_FORMAT dsvFormat = DXGI_FORMAT_UNKNOWN;
    UINT64 width = 0;
    UINT height = 0;

    static BindingSignature From(const BindingRecord& r);
    bool Matches(const BindingRecord& r) const;
    bool operator==(const BindingSignature& o) const;
    std::string ToString() const;
};

struct InjectionRules
{
    bool armed = false;
    BindingSignature gbuffer;
    int32_t gbufferOrdinal = -1;
    int32_t gbufferHdrBefore = -1; // discriminator (>= 0: HDR bindings earlier in the same list)
    BindingSignature hdr;
    int32_t hdrOrdinal = -1;
    int32_t hdrMrtBefore = -1;     // discriminator (>= 0: G-buffer bindings earlier in the same list)
    bool depthClearKnown = false;
    float depthClearValue = 0.0f; // 0 -> reversed-Z, 1 -> standard Z
    uint64_t generation = 0;
};

class FrameAnalyzer
{
public:
    // ExecuteCommandLists hook (before forwarding): appends the lists' binding logs.
    void OnExecute(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists);

    // Cheap, lock-free on the fast path (thread-local copy refreshed on generation change).
    bool MatchPrepare(const ListState& s, const BindingRecord& r) const;
    bool MatchComposite(const ListState& s, const BindingRecord& r) const;
    InjectionRules Rules() const;

    void Configure(int compositeCandidate, int gbufferOrdinal, int compositeOrdinal, int stableFrames);
    void RequestDump(const std::wstring& path);
    // Where to write a frame dump on its own, once per session, when the passes stay ambiguous for
    // kAutoDumpAfterMs (the dump is what resolves that, and testers rarely catch it with the key).
    void SetAutoDumpPath(const std::wstring& path);
    bool AutoDumpTriggered() const;

    // Human readable status line for the log
    std::string Status() const;

private:
    struct ListMarker
    {
        size_t firstRecord = 0;
        size_t recordCount = 0;
        const void* list = nullptr;
        const void* queue = nullptr;
        D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        uint32_t draws = 0;
        uint32_t tlasBuilds = 0;
        uint32_t blasBuilds = 0;
        uint32_t dispatchRays = 0;
    };

    void Analyze(bool forceDump);
    void WriteDump(size_t begin, size_t end, const InjectionRules& rules);
    const InjectionRules& CachedRules() const;

    mutable SRWLOCK m_lock = SRWLOCK_INIT;
    std::vector<BindingRecord> m_window;
    std::vector<ListMarker> m_markers;
    ULONGLONG m_lastAnalysis = 0;

    InjectionRules m_rules;
    std::atomic<uint64_t> m_generation{ 0 };

    // Stability tracking
    BindingSignature m_candGbuffer;
    int32_t m_candGbufferOrdinal = -1;
    int32_t m_candGbufferDisc = -1;
    BindingSignature m_candHdr;
    int32_t m_candHdrOrdinal = -1;
    int32_t m_candHdrDisc = -1;
    int m_stableCount = 0;

    // Configuration
    int m_compositeCandidate = 0;
    int m_gbufferOrdinalOverride = -1;
    int m_compositeOrdinalOverride = -1;
    int m_stableFrames = 3;

    bool m_dumpRequested = false;
    std::wstring m_dumpPath;
    std::wstring m_autoDumpPath;
    bool m_autoDumpDone = false;
    ULONGLONG m_ambiguousSince = 0; // tick of the first analysis in the current ambiguous run, 0 = none
    std::string m_ambiguity;        // which pass was ambiguous in the last analysis, and how ("" = none)
    std::string m_status = "waiting for frames";
};

FrameAnalyzer& Analyzer();

} // namespace rtsky::track
