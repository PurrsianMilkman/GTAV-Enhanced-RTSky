// RTSky - frame structure analysis
#include "FrameAnalyzer.h"

#include "../Common/Log.h"

#include <cstdio>
#include <string>
#include <map>

namespace rtsky::track {
namespace {

const char* FormatName(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_UNKNOWN: return "UNKNOWN";
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return "RGBA32F";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "RGBA16F";
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return "RGBA16_TYPELESS";
    case DXGI_FORMAT_R16G16B16A16_UNORM: return "RGBA16_UNORM";
    case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10F";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return "RGB10A2_UNORM";
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return "RGB10A2_TYPELESS";
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "RGBA8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "RGBA8_SRGB";
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return "RGBA8_TYPELESS";
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "BGRA8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "BGRA8_SRGB";
    case DXGI_FORMAT_R16G16_FLOAT: return "RG16F";
    case DXGI_FORMAT_R16G16_UNORM: return "RG16_UNORM";
    case DXGI_FORMAT_R16G16_SNORM: return "RG16_SNORM";
    case DXGI_FORMAT_R32_FLOAT: return "R32F";
    case DXGI_FORMAT_R16_FLOAT: return "R16F";
    case DXGI_FORMAT_R8_UNORM: return "R8_UNORM";
    case DXGI_FORMAT_R8G8_UNORM: return "RG8_UNORM";
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return "D32S8";
    case DXGI_FORMAT_D32_FLOAT: return "D32";
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24S8";
    case DXGI_FORMAT_D16_UNORM: return "D16";
    default: return nullptr;
    }
}

std::string FormatString(DXGI_FORMAT f)
{
    const char* n = FormatName(f);
    if (n != nullptr)
        return n;
    char buf[16];
    snprintf(buf, sizeof(buf), "fmt%d", static_cast<int>(f));
    return buf;
}

thread_local uint64_t t_rulesGeneration = UINT64_MAX;
thread_local InjectionRules t_rules;

} // namespace

// -------------------------------------------------------------------------------------------------
// BindingSignature
// -------------------------------------------------------------------------------------------------
BindingSignature BindingSignature::From(const BindingRecord& r)
{
    BindingSignature s;
    s.rtvCount = r.rtvCount;
    for (uint32_t i = 0; i < r.rtvCount && i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        s.rtvFormats[i] = r.rtv[i].viewFormat;
    s.hasDsv = r.hasDsv;
    s.dsvFormat = r.hasDsv ? r.dsv.viewFormat : DXGI_FORMAT_UNKNOWN;
    if (r.rtvCount > 0 && r.rtv[0].resource != nullptr)
    {
        s.width = r.rtv[0].width;
        s.height = r.rtv[0].height;
    }
    else if (r.hasDsv)
    {
        s.width = r.dsv.width;
        s.height = r.dsv.height;
    }
    return s;
}

bool BindingSignature::Matches(const BindingRecord& r) const
{
    if (r.rtvCount != rtvCount || r.hasDsv != hasDsv)
        return false;
    for (uint32_t i = 0; i < rtvCount && i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
    {
        if (r.rtv[i].viewFormat != rtvFormats[i])
            return false;
    }
    if (hasDsv && r.dsv.viewFormat != dsvFormat)
        return false;
    UINT64 w = 0;
    UINT h = 0;
    if (r.rtvCount > 0 && r.rtv[0].resource != nullptr)
    {
        w = r.rtv[0].width;
        h = r.rtv[0].height;
    }
    else if (r.hasDsv)
    {
        w = r.dsv.width;
        h = r.dsv.height;
    }
    return w == width && h == height;
}

bool BindingSignature::operator==(const BindingSignature& o) const
{
    if (rtvCount != o.rtvCount || hasDsv != o.hasDsv || dsvFormat != o.dsvFormat || width != o.width || height != o.height)
        return false;
    for (uint32_t i = 0; i < rtvCount && i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
    {
        if (rtvFormats[i] != o.rtvFormats[i])
            return false;
    }
    return true;
}

std::string BindingSignature::ToString() const
{
    std::string s = std::to_string(rtvCount) + " RT [";
    for (uint32_t i = 0; i < rtvCount && i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
    {
        if (i > 0)
            s += ",";
        s += FormatString(rtvFormats[i]);
    }
    s += "]";
    if (hasDsv)
        s += " + " + FormatString(dsvFormat);
    s += " " + std::to_string(width) + "x" + std::to_string(height);
    return s;
}

// -------------------------------------------------------------------------------------------------
// FrameAnalyzer
// -------------------------------------------------------------------------------------------------
void FrameAnalyzer::Configure(int compositeCandidate, int gbufferOrdinal, int compositeOrdinal, int stableFrames)
{
    AcquireSRWLockExclusive(&m_lock);
    if (compositeCandidate != m_compositeCandidate || gbufferOrdinal != m_gbufferOrdinalOverride ||
        compositeOrdinal != m_compositeOrdinalOverride)
    {
        m_stableCount = 0;
    }
    m_compositeCandidate = compositeCandidate < 0 ? 0 : compositeCandidate;
    m_gbufferOrdinalOverride = gbufferOrdinal;
    m_compositeOrdinalOverride = compositeOrdinal;
    m_stableFrames = stableFrames < 1 ? 1 : stableFrames;
    ReleaseSRWLockExclusive(&m_lock);
}

void FrameAnalyzer::RequestDump(const std::wstring& path)
{
    AcquireSRWLockExclusive(&m_lock);
    m_dumpRequested = true;
    m_dumpPath = path;
    ReleaseSRWLockExclusive(&m_lock);
}

void FrameAnalyzer::OnExecute(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    bool analyze = false;
    AcquireSRWLockExclusive(&m_lock);
    for (UINT i = 0; i < count; ++i)
    {
        ListState* s = FindListState(lists[i]);
        if (s == nullptr || s->type != D3D12_COMMAND_LIST_TYPE_DIRECT)
            continue;

        ListMarker marker;
        marker.firstRecord = m_window.size();
        marker.recordCount = s->log.size();
        marker.list = lists[i];
        marker.queue = queue;
        marker.type = s->type;
        marker.draws = s->draws;
        marker.tlasBuilds = s->tlasBuilds;
        marker.blasBuilds = s->blasBuilds;
        marker.dispatchRays = s->dispatchRays;
        m_markers.push_back(marker);

        m_window.insert(m_window.end(), s->log.begin(), s->log.end());
        s->log.clear(); // a list executed twice without re-recording contributes once
    }

    const ULONGLONG now = GetTickCount64();
    if (m_window.size() >= 64 && now - m_lastAnalysis >= 100)
    {
        m_lastAnalysis = now;
        analyze = true;
    }
    if (analyze)
        Analyze(false);

    // Hard cap: keep memory bounded if no frame structure is ever recognised.
    if (m_window.size() > 50000)
    {
        m_window.erase(m_window.begin(), m_window.end() - 2000);
        m_markers.clear();
    }
    ReleaseSRWLockExclusive(&m_lock);
}

// More G-buffer bindings than this in one phase: no HDR pass separates the frames any more.
static constexpr uint32_t kStalledPhaseBindings = 64;

void FrameAnalyzer::Analyze(bool forceDump)
{
    // 1. G-buffer: MRT signature with the most draws.
    struct Group
    {
        BindingSignature sig;
        uint64_t draws = 0;
        float clearDepth = 0.0f;
        bool clearKnown = false;
    };
    std::vector<Group> groups;
    for (const BindingRecord& r : m_window)
    {
        if (r.mrtOrdinal < 0)
            continue;
        BindingSignature sig = BindingSignature::From(r);
        Group* g = nullptr;
        for (Group& candidate : groups)
        {
            if (candidate.sig == sig)
            {
                g = &candidate;
                break;
            }
        }
        if (g == nullptr)
        {
            groups.push_back(Group{ sig });
            g = &groups.back();
        }
        g->draws += r.draws;
    }

    if (groups.empty())
    {
        m_status = "no G-buffer (MRT) pass seen yet";
        if (m_dumpRequested)
            WriteDump(0, m_window.size(), m_rules);
        if (m_window.size() > 8000)
        {
            m_window.clear();
            m_markers.clear();
        }
        return;
    }

    const Group* best = &groups[0];
    for (const Group& g : groups)
    {
        if (g.draws > best->draws)
            best = &g;
    }
    // Depth clear value of the G-buffer's depth (reversed-Z detection)
    bool clearKnown = false;
    float clearValue = 0.0f;
    for (const BindingRecord& r : m_window)
    {
        if (r.dsvCleared && r.hasDsv && r.dsv.viewFormat == best->sig.dsvFormat && r.dsv.height == best->sig.height)
        {
            clearKnown = true;
            clearValue = r.dsvClearDepth;
        }
    }

    // 2. G-buffer phases: maximal runs of bindings with the G-buffer signature that are not separated
    //    by a lighting-like binding (float first target of G-buffer size that is not itself an MRT
    //    pass, e.g. a decal pass on a float G-buffer). A G-buffer that is re-bound several times
    //    (opaque, decals, foliage), split across command lists or across suspended render passes
    //    forms ONE phase, and a frame is one phase plus everything up to the next phase.
    //    With GBufferOrdinal pinned, every occurrence of that binding starts a frame instead.
    const UINT64 gW = best->sig.width;
    const UINT gH = best->sig.height;
    auto isGbuffer = [&](const BindingRecord& r) { return r.mrtOrdinal >= 0 && best->sig.Matches(r); };
    auto isHdr = [&](const BindingRecord& r) { return r.hdrOrdinal >= 0 && r.rtv[0].width == gW && r.rtv[0].height == gH; };
    struct Phase
    {
        size_t first = 0;
        size_t last = 0;
        uint32_t bindings = 0;
    };
    std::vector<Phase> phases;
    if (m_gbufferOrdinalOverride >= 0)
    {
        for (size_t i = 0; i < m_window.size(); ++i)
        {
            const BindingRecord& r = m_window[i];
            if (isGbuffer(r) && r.mrtOrdinal == m_gbufferOrdinalOverride)
                phases.push_back(Phase{ i, i, 1 });
        }
    }
    else
    {
        bool litSincePhase = true;
        for (size_t i = 0; i < m_window.size(); ++i)
        {
            const BindingRecord& r = m_window[i];
            if (isGbuffer(r))
            {
                if (litSincePhase)
                    phases.push_back(Phase{ i, i, 0 });
                phases.back().last = i;
                ++phases.back().bindings;
                litSincePhase = false;
            }
            else if (isHdr(r) && r.mrtOrdinal < 0)
            {
                litSincePhase = true;
            }
        }
    }
    if (phases.empty())
    {
        m_status = "G-buffer " + best->sig.ToString() + " found, but no binding with GBufferOrdinal=" +
                   std::to_string(m_gbufferOrdinalOverride);
        if (m_dumpRequested || forceDump)
            WriteDump(0, m_window.size(), m_rules);
        return;
    }
    // Without HDR passes, consecutive frames merge into one phase (menus and loading screens that
    // still render a G-buffer but light nothing). A phase that keeps growing means exactly that.
    if (phases.back().bindings > kStalledPhaseBindings)
    {
        if (m_rules.armed)
        {
            m_rules.armed = false;
            m_rules.generation = m_generation.load() + 1;
            m_generation.store(m_rules.generation, std::memory_order_release);
            LOG_INFO("Frame analysis: no HDR lighting pass in recent frames, injection disarmed");
        }
        m_stableCount = 0;
        m_status = "G-buffer " + best->sig.ToString() + " without an HDR pass (menu / loading?)";
        if (m_dumpRequested || forceDump)
            WriteDump(0, m_window.size(), m_rules);
        const size_t keepFrom = phases.back().last;
        m_window.erase(m_window.begin(), m_window.begin() + static_cast<std::ptrdiff_t>(keepFrom));
        m_markers.clear();
        return;
    }
    if (phases.size() < 2)
    {
        m_status = "G-buffer " + best->sig.ToString() + " found, waiting for a complete frame";
        if (m_dumpRequested || forceDump)
            WriteDump(0, m_window.size(), m_rules);
        return;
    }

    // 3. Per complete frame: the injection bindings and rules that match each of them exactly once.
    //    A rule is (signature, ordinal within its list); when that is ambiguous (the same pass
    //    signature at the same ordinal in two lists), the number of G-buffer (resp. HDR) bindings
    //    recorded earlier in the same list is added as a discriminator.
    int framesWithoutHdr = 0;
    int ambiguousFrames = 0;
    for (size_t k = 0; k + 1 < phases.size(); ++k)
    {
        const size_t frameBegin = phases[k].first;
        const size_t frameEnd = phases[k + 1].first;

        // Prepare: the LAST binding of the phase that writes depth (the depth is complete only
        // there; read-only-depth re-binds such as decals do not change it), or the pinned binding.
        const BindingRecord* g = nullptr;
        for (size_t i = phases[k].first; i <= phases[k].last; ++i)
        {
            const BindingRecord& r = m_window[i];
            if (!isGbuffer(r))
                continue;
            if (m_gbufferOrdinalOverride >= 0 ? r.mrtOrdinal == m_gbufferOrdinalOverride : !r.dsvReadOnlyDepth)
                g = &r;
        }

        // Composite: the CompositeCandidate-th HDR binding after the phase (or the one with the
        // configured ordinal), with its own signature.
        std::vector<const BindingRecord*> candidates;
        for (size_t i = phases[k].last + 1; i < frameEnd; ++i)
        {
            if (isHdr(m_window[i]))
                candidates.push_back(&m_window[i]);
        }
        const BindingRecord* h = nullptr;
        if (m_compositeOrdinalOverride >= 0)
        {
            for (const BindingRecord* c : candidates)
            {
                if (c->hdrOrdinal == m_compositeOrdinalOverride)
                {
                    h = c;
                    break;
                }
            }
        }
        else if (candidates.size() > static_cast<size_t>(m_compositeCandidate))
        {
            h = candidates[m_compositeCandidate];
        }
        if (g == nullptr || h == nullptr)
        {
            ++framesWithoutHdr;
            m_stableCount = 0;
            continue;
        }

        const BindingSignature gSig = BindingSignature::From(*g);
        const BindingSignature hSig = BindingSignature::From(*h);
        auto countIn = [&](auto&& pred) {
            int n = 0;
            for (size_t i = frameBegin; i < frameEnd; ++i)
                n += pred(m_window[i]) ? 1 : 0;
            return n;
        };
        int32_t gDisc = -1;
        if (countIn([&](const BindingRecord& r) { return r.mrtOrdinal == g->mrtOrdinal && gSig.Matches(r); }) != 1)
        {
            gDisc = static_cast<int32_t>(g->hdrBefore);
            if (countIn([&](const BindingRecord& r) {
                    return r.mrtOrdinal == g->mrtOrdinal && static_cast<int32_t>(r.hdrBefore) == gDisc && gSig.Matches(r);
                }) != 1)
                gDisc = -2;
        }
        int32_t hDisc = -1;
        if (countIn([&](const BindingRecord& r) { return r.hdrOrdinal == h->hdrOrdinal && hSig.Matches(r); }) != 1)
        {
            hDisc = static_cast<int32_t>(h->mrtBefore);
            if (countIn([&](const BindingRecord& r) {
                    return r.hdrOrdinal == h->hdrOrdinal && static_cast<int32_t>(r.mrtBefore) == hDisc && hSig.Matches(r);
                }) != 1)
                hDisc = -2;
        }
        if (gDisc == -2 || hDisc == -2)
        {
            // No rule identifies the pass uniquely: injecting could hit the wrong pass (or twice).
            ++ambiguousFrames;
            m_stableCount = 0;
            continue;
        }

        if (gSig == m_candGbuffer && g->mrtOrdinal == m_candGbufferOrdinal && gDisc == m_candGbufferDisc && hSig == m_candHdr &&
            h->hdrOrdinal == m_candHdrOrdinal && hDisc == m_candHdrDisc)
        {
            ++m_stableCount;
        }
        else
        {
            m_candGbuffer = gSig;
            m_candGbufferOrdinal = g->mrtOrdinal;
            m_candGbufferDisc = gDisc;
            m_candHdr = hSig;
            m_candHdrOrdinal = h->hdrOrdinal;
            m_candHdrDisc = hDisc;
            m_stableCount = 1;
        }
    }

    if (m_stableCount >= m_stableFrames)
    {
        bool changed = !m_rules.armed || !(m_rules.gbuffer == m_candGbuffer) || m_rules.gbufferOrdinal != m_candGbufferOrdinal ||
                       m_rules.gbufferHdrBefore != m_candGbufferDisc || !(m_rules.hdr == m_candHdr) ||
                       m_rules.hdrOrdinal != m_candHdrOrdinal || m_rules.hdrMrtBefore != m_candHdrDisc ||
                       m_rules.depthClearKnown != clearKnown || (clearKnown && m_rules.depthClearValue != clearValue);
        if (changed)
        {
            m_rules.armed = true;
            m_rules.gbuffer = m_candGbuffer;
            m_rules.gbufferOrdinal = m_candGbufferOrdinal;
            m_rules.gbufferHdrBefore = m_candGbufferDisc;
            m_rules.hdr = m_candHdr;
            m_rules.hdrOrdinal = m_candHdrOrdinal;
            m_rules.hdrMrtBefore = m_candHdrDisc;
            m_rules.depthClearKnown = clearKnown;
            m_rules.depthClearValue = clearValue;
            m_rules.generation = m_generation.load() + 1;
            m_generation.store(m_rules.generation, std::memory_order_release);
            LOG_INFO("Frame analysis: G-buffer = %s (list ordinal %d%s), HDR lighting = %s (list ordinal %d%s), depth clear %s%.1f",
                     m_rules.gbuffer.ToString().c_str(), m_rules.gbufferOrdinal, m_rules.gbufferHdrBefore >= 0 ? ", discriminated" : "",
                     m_rules.hdr.ToString().c_str(), m_rules.hdrOrdinal, m_rules.hdrMrtBefore >= 0 ? ", discriminated" : "",
                     clearKnown ? "" : "unknown ", clearValue);
        }
        m_status = "armed";
    }
    else if (m_rules.armed && (framesWithoutHdr > 0 || ambiguousFrames > 0) && m_stableCount == 0)
    {
        // The frame structure changed (menu, loading screen, settings change) or the rules stopped
        // identifying the passes uniquely: disarm until stable again.
        m_rules.armed = false;
        m_rules.generation = m_generation.load() + 1;
        m_generation.store(m_rules.generation, std::memory_order_release);
        m_status = "frame structure changed, re-analysing";
        LOG_INFO("Frame analysis: %s, injection disarmed",
                 ambiguousFrames > 0 ? "the passes are no longer identified uniquely" : "HDR lighting pass not found any more");
    }
    else if (!m_rules.armed)
    {
        if (ambiguousFrames > 0)
            m_status = "G-buffer " + best->sig.ToString() + ": the G-buffer or HDR pass is not unique per frame "
                       "(press the frame-dump key and see docs/CALIBRATION.md)";
        else
            m_status = "G-buffer " + best->sig.ToString() + ", HDR pass " +
                       (m_stableCount > 0 ? "found, stabilising (if this persists, pin GBufferOrdinal from the frame dump)"
                                          : "not found (check CompositeCandidate)");
    }

    if (m_dumpRequested || forceDump)
        WriteDump(phases[phases.size() - 2].first, phases.back().first, m_rules);

    // Keep the incomplete tail (the last phase and what follows) for the next analysis.
    const size_t keepFrom = phases.back().first;
    std::vector<ListMarker> keptMarkers;
    for (const ListMarker& m : m_markers)
    {
        if (m.firstRecord + m.recordCount > keepFrom)
        {
            ListMarker adjusted = m;
            adjusted.firstRecord = m.firstRecord >= keepFrom ? m.firstRecord - keepFrom : 0;
            keptMarkers.push_back(adjusted);
        }
    }
    m_window.erase(m_window.begin(), m_window.begin() + static_cast<std::ptrdiff_t>(keepFrom));
    m_markers.swap(keptMarkers);
}

void FrameAnalyzer::WriteDump(size_t begin, size_t end, const InjectionRules& rules)
{
    m_dumpRequested = false;
    FILE* f = _wfopen(m_dumpPath.c_str(), L"w");
    if (f == nullptr)
    {
        LOG_ERROR("Could not write the frame dump");
        return;
    }
    fprintf(f, "RTSky frame dump - %zu bindings (execution order)\n", end - begin);
    fprintf(f, "Rules: %s\n", rules.armed ? "armed" : "not armed");
    fprintf(f, "  G-buffer : %s, list ordinal %d\n", rules.gbuffer.ToString().c_str(), rules.gbufferOrdinal);
    fprintf(f, "  Composite: %s, list ordinal %d\n", rules.hdr.ToString().c_str(), rules.hdrOrdinal);
    fprintf(f, "  Depth clear value: %s %.3f\n\n", rules.depthClearKnown ? "" : "(unknown)", rules.depthClearValue);

    size_t markerIndex = 0;
    for (size_t i = begin; i < end && i < m_window.size(); ++i)
    {
        while (markerIndex < m_markers.size() && m_markers[markerIndex].firstRecord <= i)
        {
            const ListMarker& m = m_markers[markerIndex++];
            if (m.firstRecord + m.recordCount <= begin)
                continue;
            fprintf(f, "---- list %p (queue %p): %u draws, %u TLAS builds, %u BLAS builds, %u DispatchRays\n", m.list,
                    m.queue, m.draws, m.tlasBuilds, m.blasBuilds, m.dispatchRays);
        }
        const BindingRecord& r = m_window[i];
        fprintf(f, "  #%-3u %-60s draws %-5u", r.listSeq, BindingSignature::From(r).ToString().c_str(), r.draws);
        if (r.mrtOrdinal >= 0)
            fprintf(f, " mrt#%d (after %u hdr)", r.mrtOrdinal, r.hdrBefore);
        if (r.hdrOrdinal >= 0)
            fprintf(f, " hdr#%d (after %u mrt)", r.hdrOrdinal, r.mrtBefore);
        if (r.hasDsv)
            fprintf(f, " dsv%s%s", r.dsvReadOnlyDepth ? " ro-depth" : "", r.dsvReadOnlyStencil ? " ro-stencil" : "");
        if (r.dsvCleared)
            fprintf(f, " clear=%.2f", r.dsvClearDepth);
        if (r.viewportValid)
            fprintf(f, " vp=%.0f,%.0f %.0fx%.0f", r.viewport.TopLeftX, r.viewport.TopLeftY, r.viewport.Width, r.viewport.Height);
        if (r.fromRenderPass)
            fprintf(f, " renderpass");
        if (rules.armed && r.mrtOrdinal == rules.gbufferOrdinal &&
            (rules.gbufferHdrBefore < 0 || static_cast<int32_t>(r.hdrBefore) == rules.gbufferHdrBefore) && rules.gbuffer.Matches(r))
            fprintf(f, "   <== PREPARE");
        if (rules.armed && r.hdrOrdinal == rules.hdrOrdinal &&
            (rules.hdrMrtBefore < 0 || static_cast<int32_t>(r.mrtBefore) == rules.hdrMrtBefore) && rules.hdr.Matches(r))
            fprintf(f, "   <== COMPOSITE");
        fprintf(f, "\n");
    }
    fclose(f);
    LOG_INFO("Frame dump written");
}

const InjectionRules& FrameAnalyzer::CachedRules() const
{
    const uint64_t gen = m_generation.load(std::memory_order_acquire);
    if (gen != t_rulesGeneration)
    {
        AcquireSRWLockShared(&m_lock);
        t_rules = m_rules;
        t_rulesGeneration = m_rules.generation;
        ReleaseSRWLockShared(&m_lock);
    }
    return t_rules;
}

bool FrameAnalyzer::MatchPrepare(const ListState& s, const BindingRecord& r) const
{
    if (s.type != D3D12_COMMAND_LIST_TYPE_DIRECT || r.mrtOrdinal < 0)
        return false;
    const InjectionRules& rules = CachedRules();
    return rules.armed && r.mrtOrdinal == rules.gbufferOrdinal &&
           (rules.gbufferHdrBefore < 0 || static_cast<int32_t>(r.hdrBefore) == rules.gbufferHdrBefore) && rules.gbuffer.Matches(r);
}

bool FrameAnalyzer::MatchComposite(const ListState& s, const BindingRecord& r) const
{
    if (s.type != D3D12_COMMAND_LIST_TYPE_DIRECT || r.hdrOrdinal < 0)
        return false;
    const InjectionRules& rules = CachedRules();
    return rules.armed && r.hdrOrdinal == rules.hdrOrdinal &&
           (rules.hdrMrtBefore < 0 || static_cast<int32_t>(r.mrtBefore) == rules.hdrMrtBefore) && rules.hdr.Matches(r);
}

InjectionRules FrameAnalyzer::Rules() const
{
    AcquireSRWLockShared(&m_lock);
    InjectionRules copy = m_rules;
    ReleaseSRWLockShared(&m_lock);
    return copy;
}

std::string FrameAnalyzer::Status() const
{
    AcquireSRWLockShared(&m_lock);
    std::string s = m_status;
    ReleaseSRWLockShared(&m_lock);
    return s;
}

FrameAnalyzer& Analyzer()
{
    // Never destroyed: hooks and GPU-lifetime deleters may still run during process exit, after
    // static destructors (destruction order across translation units is unspecified).
    static FrameAnalyzer* instance = new FrameAnalyzer();
    return *instance;
}

} // namespace rtsky::track
