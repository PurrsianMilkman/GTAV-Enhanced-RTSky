// RTSky - host tests of the frame analyzer (G-buffer phases, rule uniqueness, discriminators).
//
// Builds on Linux against DirectX-Headers' WSL stubs plus tests/shim (SRW locks, tick count): the
// tracker and analyzer only call a handful of COM methods, which a fake command-list vtable provides.
// See tests/CMakeLists.txt / RTSKY_BUILD_ANALYZER_TESTS.
#include "Track/CommandListTracker.h"
#include "Track/FrameAnalyzer.h"
#include "Common/Log.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

unsigned long long g_testTick = 1000;

namespace rtsky::log {
void Init(const wchar_t*) {}
void Shutdown() {}
void SetLevel(Level) {}
bool Enabled(Level) { return false; }
void WriteV(Level, const char*, va_list) {}
void Write(Level, const char*, ...) {}
} // namespace rtsky::log

using namespace rtsky::track;

// -------------------------------------------------------------------------------------------------
// Fake command list: only the IUnknown / ID3D12Object / ID3D12CommandList slots the tracker calls.
// -------------------------------------------------------------------------------------------------
namespace {

HRESULT FakeQueryInterface(void*, REFIID, void** out)
{
    *out = nullptr;
    return E_NOINTERFACE;
}
ULONG FakeAddRef(void*) { return 1; }
ULONG FakeRelease(void*) { return 1; }
HRESULT FakeSetPrivateDataInterface(void*, REFGUID, const IUnknown* data)
{
    // The runtime keeps a reference; tests never destroy lists.
    if (data != nullptr)
        const_cast<IUnknown*>(data)->AddRef();
    return S_OK;
}
D3D12_COMMAND_LIST_TYPE FakeGetType(void*) { return D3D12_COMMAND_LIST_TYPE_DIRECT; }
void FakeUnused(void*) { std::fprintf(stderr, "unexpected COM call\n"); std::abort(); }

struct FakeList
{
    void** vtable;
};

void** FakeVtable()
{
    static void* table[256];
    static bool init = false;
    if (!init)
    {
        for (void*& p : table)
            p = reinterpret_cast<void*>(&FakeUnused);
        table[0] = reinterpret_cast<void*>(&FakeQueryInterface);
        table[1] = reinterpret_cast<void*>(&FakeAddRef);
        table[2] = reinterpret_cast<void*>(&FakeRelease);
        table[5] = reinterpret_cast<void*>(&FakeSetPrivateDataInterface);
        table[8] = reinterpret_cast<void*>(&FakeGetType);
        init = true;
    }
    return table;
}

std::vector<std::unique_ptr<FakeList>> g_fakes;

ID3D12GraphicsCommandList* NewList()
{
    g_fakes.push_back(std::make_unique<FakeList>(FakeList{ FakeVtable() }));
    return reinterpret_cast<ID3D12GraphicsCommandList*>(g_fakes.back().get());
}

ID3D12Resource* const kGbufferRt = reinterpret_cast<ID3D12Resource*>(0x1000);
ID3D12Resource* const kHdrRt = reinterpret_cast<ID3D12Resource*>(0x2000);
ID3D12Resource* const kDepth = reinterpret_cast<ID3D12Resource*>(0x3000);

BoundTarget Target(ID3D12Resource* r, DXGI_FORMAT f)
{
    BoundTarget t;
    t.resource = r;
    t.viewFormat = f;
    t.resourceFormat = f;
    t.width = 2560;
    t.height = 1440;
    return t;
}

// Pass kinds: 'G' G-buffer (4 x RGBA8 + depth), 'g' the same with read-only depth (decals),
// 'F' a float MRT pass (3 x RGBA16F + depth), 'H' a float HDR pass (1 x RGBA16F + depth).
void Bind(ListState& s, char kind, uint32_t draws)
{
    s.current = BindingRecord{};
    BindingRecord& r = s.current;
    if (kind == 'G' || kind == 'g')
    {
        r.rtvCount = 4;
        for (int i = 0; i < 4; ++i)
            r.rtv[i] = Target(kGbufferRt, DXGI_FORMAT_R8G8B8A8_UNORM);
        r.dsvReadOnlyDepth = kind == 'g';
    }
    else if (kind == 'F')
    {
        r.rtvCount = 3;
        for (int i = 0; i < 3; ++i)
            r.rtv[i] = Target(kHdrRt, DXGI_FORMAT_R16G16B16A16_FLOAT);
    }
    else
    {
        r.rtvCount = 1;
        r.rtv[0] = Target(kHdrRt, DXGI_FORMAT_R16G16B16A16_FLOAT);
    }
    r.hasDsv = true;
    r.dsv = Target(kDepth, DXGI_FORMAT_D32_FLOAT_S8X24_UINT);
    r.draws = draws;
    s.bindingOpen = true;
    CloseBinding(s);
}

void Bind(ListState& s, bool mrt, uint32_t draws)
{
    Bind(s, mrt ? 'G' : 'H', draws);
}

// A frame: a sequence of lists, each a string of pass kinds (see Bind).
void RunFrame(const std::vector<std::pair<ID3D12GraphicsCommandList*, const char*>>& lists)
{
    std::vector<ID3D12CommandList*> submit;
    for (const auto& [list, passes] : lists)
    {
        ListState& s = *GetListState(list);
        OnReset(s, nullptr);
        for (const char* p = passes; *p != 0; ++p)
            Bind(s, *p, *p == 'G' ? 500 : 20);
        submit.push_back(list);
    }
    g_testTick += 16;
    Analyzer().OnExecute(nullptr, static_cast<UINT>(submit.size()), submit.data());
}

int g_failures = 0;

void Check(bool ok, const char* what)
{
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++g_failures;
}

void Frames(int n, const std::vector<std::pair<ID3D12GraphicsCommandList*, const char*>>& lists)
{
    for (int i = 0; i < n; ++i)
        RunFrame(lists);
}

// The analyzer is a singleton: each scenario uses fresh lists and runs enough frames to re-arm.
void Scenario(const char* name)
{
    std::printf("%s\n", name);
}

} // namespace

int main()
{
    Analyzer().Configure(0, -1, -1, 3);

    {
        Scenario("single list: G-buffer then lighting then a second HDR pass");
        ID3D12GraphicsCommandList* a = NewList();
        Frames(60, { { a, "GHH" } });
        const InjectionRules r = Analyzer().Rules();
        Check(r.armed, "armed");
        Check(r.gbufferOrdinal == 0 && r.gbufferHdrBefore == -1, "Prepare at mrt#0, no discriminator");
        Check(r.hdrOrdinal == 0 && r.hdrMrtBefore == -1, "Composite at hdr#0, no discriminator");
    }
    {
        Scenario("G-buffer re-bound three times (opaque, decals, foliage): Prepare after the last");
        ID3D12GraphicsCommandList* a = NewList();
        Frames(60, { { a, "GGGHH" } });
        const InjectionRules r = Analyzer().Rules();
        Check(r.armed, "armed");
        Check(r.gbufferOrdinal == 2, "Prepare at mrt#2");
        Check(r.hdrOrdinal == 0, "Composite at hdr#0");
    }
    {
        Scenario("decals re-bind the G-buffer with read-only depth: Prepare stays on the depth-writing binding");
        ID3D12GraphicsCommandList* a = NewList();
        Frames(60, { { a, "GgH" } });
        const InjectionRules r = Analyzer().Rules();
        Check(r.armed && r.gbufferOrdinal == 0, "Prepare at mrt#0 (not the read-only mrt#1)");
    }
    {
        Scenario("a float MRT pass inside the G-buffer phase does not split the frame");
        ID3D12GraphicsCommandList* a = NewList();
        Frames(60, { { a, "GFGH" } });
        const InjectionRules r = Analyzer().Rules();
        Check(r.armed, "armed");
        Check(r.gbufferOrdinal == 2, "Prepare at mrt#2 (after the float MRT pass)");
        Check(r.hdrOrdinal == 1, "Composite at hdr#1 (the float MRT pass is hdr#0)");
    }
    {
        Scenario("a lighting-like pass between two G-buffer binds: pinning GBufferOrdinal delimits frames");
        Analyzer().Configure(0, 1, -1, 3);
        ID3D12GraphicsCommandList* a = NewList();
        Frames(60, { { a, "GHGHH" } });
        const InjectionRules r = Analyzer().Rules();
        Check(r.armed && r.gbufferOrdinal == 1 && r.hdrOrdinal == 1, "Prepare at mrt#1, Composite at hdr#1");
        Analyzer().Configure(0, -1, -1, 3);
    }
    {
        Scenario("CompositeCandidate=1 picks the second HDR pass");
        Analyzer().Configure(1, -1, -1, 3);
        ID3D12GraphicsCommandList* a = NewList();
        Frames(60, { { a, "GHH" } });
        const InjectionRules r = Analyzer().Rules();
        Check(r.armed && r.hdrOrdinal == 1, "Composite at hdr#1");
        Analyzer().Configure(0, -1, -1, 3);
    }
    {
        Scenario("look-alike HDR pass at hdr#0 in another list: discriminated by G-buffer passes before it");
        ID3D12GraphicsCommandList* a = NewList();
        ID3D12GraphicsCommandList* b = NewList();
        Frames(60, { { a, "GH" }, { b, "H" } });
        const InjectionRules r = Analyzer().Rules();
        Check(r.armed, "armed");
        Check(r.hdrOrdinal == 0 && r.hdrMrtBefore == 1, "Composite at hdr#0 after 1 G-buffer pass in the list");
    }
    {
        // GTA V Enhanced (v0.1.2 frame dump): ~15 parallel lists each bind the G-buffer at mrt#0,
        // one of them followed by other MRT passes; the lighting is in later lists.
        Scenario("G-buffer recorded in parallel lists (same ordinal): Prepare after every one of them");
        ID3D12GraphicsCommandList* a = NewList();
        ID3D12GraphicsCommandList* b = NewList();
        ID3D12GraphicsCommandList* c = NewList();
        ID3D12GraphicsCommandList* d = NewList();
        Frames(60, { { a, "G" }, { b, "G" }, { c, "Gg" }, { d, "HH" } });
        const InjectionRules r = Analyzer().Rules();
        Check(r.armed, "armed");
        Check(r.gbufferEvery && r.gbufferOrdinal == 0, "Prepare after every mrt#0 (every-list mode)");
        Check(r.hdrOrdinal == 0, "Composite at hdr#0");
        ListState& sa = *GetListState(a);
        OnReset(sa, nullptr);
        Bind(sa, 'G', 500);
        Check(Analyzer().MatchPrepare(sa, sa.log.back()), "Prepare matches list A's G-buffer");
        ListState& sc = *GetListState(c);
        OnReset(sc, nullptr);
        Bind(sc, 'G', 500);
        const BindingRecord gc = sc.log.back();
        Bind(sc, 'g', 20);
        const BindingRecord gro = sc.log.back();
        Check(Analyzer().MatchPrepare(sc, gc), "Prepare matches list C's G-buffer");
        Check(!Analyzer().MatchPrepare(sc, gro), "no Prepare after the read-only-depth re-bind");
    }
    {
        Scenario("look-alike lighting passes at hdr#0 in two lists after the G-buffer: not unique -> never armed");
        Analyzer().SetAutoDumpPath(L"RTSky_frame.log"); // _wfopen is a stub here: nothing is written
        ID3D12GraphicsCommandList* a = NewList();
        ID3D12GraphicsCommandList* b = NewList();
        ID3D12GraphicsCommandList* c = NewList();
        Frames(60, { { a, "G" }, { b, "H" }, { c, "H" } });
        const InjectionRules r = Analyzer().Rules();
        Check(!r.armed, "not armed");
        Check(Analyzer().Status().find("not unique") != std::string::npos, "status explains the ambiguity");
        Check(Analyzer().Status().find("HDR hdr#0") != std::string::npos && Analyzer().Status().find(" x2 per frame") != std::string::npos,
              "status names the ambiguous pass and its count");
        Check(Analyzer().Status().find("G-buffer mrt#") == std::string::npos, "status does not blame the unique G-buffer");
        Check(!Analyzer().AutoDumpTriggered(), "no automatic frame dump within the first second");
        Frames(240, { { a, "G" }, { b, "H" }, { c, "H" } }); // ~4 s more
        Check(Analyzer().AutoDumpTriggered(), "automatic frame dump once ambiguous for 3 s");
        Analyzer().SetAutoDumpPath(L"");
    }
    {
        Scenario("...unless GBufferOrdinal pins a binding that is unique (list B binds it twice)");
        Analyzer().Configure(0, 1, -1, 3);
        ID3D12GraphicsCommandList* a = NewList();
        ID3D12GraphicsCommandList* b = NewList();
        Frames(60, { { a, "G" }, { b, "GGH" } });
        const InjectionRules r = Analyzer().Rules();
        Check(r.armed && r.gbufferOrdinal == 1, "Prepare at mrt#1");
        Analyzer().Configure(0, -1, -1, 3);
    }
    {
        Scenario("menu: frames without an HDR pass disarm, gameplay re-arms");
        ID3D12GraphicsCommandList* a = NewList();
        Frames(60, { { a, "GHH" } });
        Check(Analyzer().Rules().armed, "armed in gameplay");
        Frames(120, { { a, "G" } }); // ~2 s of menu: more than 64 G-buffer bindings without lighting
        Check(!Analyzer().Rules().armed, "disarmed in the menu");
        Frames(60, { { a, "GHH" } });
        Check(Analyzer().Rules().armed, "re-armed");
    }

    // Matching at recording time
    {
        Scenario("recording-time matching follows the armed rules");
        ID3D12GraphicsCommandList* a = NewList();
        ID3D12GraphicsCommandList* b = NewList();
        Frames(60, { { a, "GH" }, { b, "H" } });
        ListState& sa = *GetListState(a);
        OnReset(sa, nullptr);
        Bind(sa, true, 500);
        const BindingRecord ga = sa.log.back();
        Bind(sa, false, 20);
        const BindingRecord ha = sa.log.back();
        ListState& sb = *GetListState(b);
        OnReset(sb, nullptr);
        Bind(sb, false, 20);
        const BindingRecord hb = sb.log.back();
        Check(Analyzer().MatchPrepare(sa, ga), "Prepare matches list A's G-buffer");
        Check(Analyzer().MatchComposite(sa, ha), "Composite matches list A's lighting");
        Check(!Analyzer().MatchComposite(sb, hb), "Composite does not match list B's look-alike pass");
    }

    std::printf(g_failures == 0 ? "\nall analyzer tests passed\n" : "\n%d analyzer test(s) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
