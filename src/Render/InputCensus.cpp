// RTSky - input census of the game's deferred lighting pass (PS_directional_standard)
#include "InputCensus.h"

#include "../Common/D3D12Compat.h"
#include "../Common/Log.h"
#include "../Game/GameData.h"
#include "../Track/BufferTracker.h"
#include "../Track/CommandListTracker.h"
#include "../Track/DescriptorTracker.h"
#include "../Track/PassNames.h"
#include "../Track/RootSignatureTracker.h"
#include "../Track/ShaderCapture.h"

#include <wrl/client.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

namespace rtsky::render {
namespace {

using Microsoft::WRL::ComPtr;
using track::ListState;

constexpr ULONGLONG kGameCbInterval = 10000;

std::atomic<ULONGLONG> g_nextCensus{ 0 };
std::atomic<bool> g_fullDone{ false };
std::atomic<bool> g_busy{ false };

// The registers of PS_directional_standard RTSky's replacement reads (LIGHTING-REPORT.md section 1).
struct Reg
{
    const char* name;
    D3D12_DESCRIPTOR_RANGE_TYPE kind;
    uint32_t reg;
    uint32_t space;
};
const Reg kSrvs[] = {
    { "t12 G-buffer0 (albedo)", D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 12, 0 },
    { "t13 G-buffer1 (normal)", D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 13, 0 },
    { "t14 G-buffer2 (spec, shadow)", D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 14, 0 },
    { "t15 G-buffer3 (ambient scales)", D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 15, 0 },
    { "t16 stencil", D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 16, 0 },
    { "t17 depth", D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 17, 0 },
    { "t10,s1 ao", D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 10, 1 },
    { "t24,s1 RTGI", D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 24, 1 },
};
enum CbIndex { CB_Misc, CB_Lighting, CB_Locals, CB_Count };
const Reg kCbs[CB_Count] = {
    { "cb4 misc_globals", D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 4, 0 },
    { "cb5 lighting_globals", D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 5, 0 },
    { "cb10,s1 lighting_locals", D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 10, 1 },
};

struct Resolved
{
    bool ok = false;
    std::string how;   // "root p0" / "table p3+12"
    std::string fail;  // why not
    track::ViewInfo view;
    D3D12_GPU_VIRTUAL_ADDRESS va = 0; // CBV
};

UINT CbvSrvUavIncrement(const ListState& s)
{
    static std::atomic<UINT> cached{ 0 };
    UINT inc = cached.load(std::memory_order_relaxed);
    if (inc == 0 && s.list != nullptr)
    {
        ComPtr<ID3D12Device> device;
        if (SUCCEEDED(s.list->GetDevice(IID_PPV_ARGS(&device))))
        {
            inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            cached.store(inc, std::memory_order_relaxed);
        }
    }
    return inc;
}

// GPU handle of a bound shader-visible CBV_SRV_UAV heap -> the CPU handle of the same descriptor.
bool CpuHandleFor(const ListState& s, uint64_t gpu, D3D12_CPU_DESCRIPTOR_HANDLE* cpu)
{
    const UINT inc = CbvSrvUavIncrement(s);
    if (inc == 0)
        return false;
    for (uint32_t i = 0; i < s.heapCount && i < 2; ++i)
    {
        ID3D12DescriptorHeap* heap = s.heaps[i];
        if (heap == nullptr)
            continue;
        const D3D12_DESCRIPTOR_HEAP_DESC desc = DescriptorHeapDesc(heap);
        if (desc.Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
            continue;
        const uint64_t start = HeapGpuStart(heap).ptr;
        if (gpu < start || (gpu - start) / inc >= desc.NumDescriptors)
            continue;
        cpu->ptr = HeapCpuStart(heap).ptr + static_cast<SIZE_T>(gpu - start);
        return true;
    }
    return false;
}

Resolved Resolve(const ListState& s, const track::RootLayout& layout, const Reg& r)
{
    Resolved out;
    track::RootLocation loc;
    if (!track::ResolveRegister(layout, r.kind, r.reg, r.space, &loc))
    {
        out.fail = "not in the root signature";
        return out;
    }
    if (loc.param < 0 || static_cast<uint32_t>(loc.param) >= track::BindPointState::kMaxRootParams)
    {
        out.fail = "root parameter out of range";
        return out;
    }
    const track::BindPointState::Arg& arg = s.graphics.args[loc.param];
    char buf[64];
    if (loc.descriptorOffset < 0)
    {
        snprintf(buf, sizeof(buf), "root p%d", loc.param);
        out.how = buf;
        if (arg.type == track::BindPointState::ArgNone || arg.value == 0)
        {
            out.fail = "root descriptor not set";
            return out;
        }
        out.va = arg.value;
        if (r.kind == D3D12_DESCRIPTOR_RANGE_TYPE_CBV)
        {
            out.view.kind = track::ViewInfo::Kind::CBV;
            out.view.cbvLocation = arg.value;
        }
        out.ok = true;
        return out;
    }
    snprintf(buf, sizeof(buf), "table p%d+%lld", loc.param, static_cast<long long>(loc.descriptorOffset));
    out.how = buf;
    if (arg.type != track::BindPointState::ArgTable || arg.value == 0)
    {
        out.fail = "descriptor table not set";
        return out;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = {};
    if (!CpuHandleFor(s, arg.value + static_cast<uint64_t>(loc.descriptorOffset) * CbvSrvUavIncrement(s), &cpu))
    {
        out.fail = "table outside the bound descriptor heaps";
        return out;
    }
    if (!track::Descriptors().Lookup(cpu, &out.view))
    {
        out.fail = track::Descriptors().TrackShaderViews() ? "descriptor not tracked (created before RTSky hooked, or by a path RTSky does not see)"
                                                           : "descriptor tracking off (TrackShaderDescriptors=0)";
        return out;
    }
    if (out.view.kind == track::ViewInfo::Kind::CBV)
        out.va = out.view.cbvLocation;
    out.ok = true;
    return out;
}

const char* HeapName(D3D12_HEAP_TYPE t)
{
    switch (t)
    {
    case D3D12_HEAP_TYPE_DEFAULT: return "DEFAULT";
    case D3D12_HEAP_TYPE_UPLOAD: return "UPLOAD";
    case D3D12_HEAP_TYPE_READBACK: return "READBACK";
    case D3D12_HEAP_TYPE_CUSTOM: return "CUSTOM";
    default: return "?";
    }
}

std::string DescribeSrv(const ListState& s, const Reg& r, const Resolved& res)
{
    char buf[512];
    if (!res.ok)
    {
        snprintf(buf, sizeof(buf), "%s: %s%s%s", r.name, res.how.c_str(), res.how.empty() ? "" : ": ", res.fail.c_str());
        return buf;
    }
    if (res.view.kind != track::ViewInfo::Kind::SRV || res.view.resource == nullptr)
    {
        snprintf(buf, sizeof(buf), "%s: %s: not a texture SRV (kind %d)", r.name, res.how.c_str(), static_cast<int>(res.view.kind));
        return buf;
    }
    // The game has this resource bound for the draw being recorded: it is alive.
    ID3D12Resource* resource = res.view.resource;
    const D3D12_RESOURCE_DESC d = ResourceDesc(resource);
    std::string state;
    track::ObservedState o;
    if (track::FindObservedState(s, resource, 0, &o))
    {
        char st[96];
        snprintf(st, sizeof(st), "barrier in this list (seq %u): %s 0x%X%s", o.seq, o.enhanced ? "layout" : "state", o.stateOrLayout,
                 o.splitPending ? ", split pending" : "");
        state = st;
    }
    else
    {
        state = "no barrier in this list";
    }
    snprintf(buf, sizeof(buf), "%s: %s -> %p fmt %d (view %d, dim %d, plane %u) %llux%u arr %u mips %u flags 0x%X | %s", r.name, res.how.c_str(),
             static_cast<void*>(resource), static_cast<int>(d.Format), static_cast<int>(res.view.viewFormat), res.view.viewDimension,
             res.view.planeSlice, static_cast<unsigned long long>(d.Width), d.Height, d.DepthOrArraySize, d.MipLevels,
             static_cast<unsigned>(d.Flags), state.c_str());
    return buf;
}

std::string DescribeCb(const Reg& r, const Resolved& res)
{
    char buf[384];
    if (!res.ok)
    {
        snprintf(buf, sizeof(buf), "%s: %s%s%s", r.name, res.how.c_str(), res.how.empty() ? "" : ": ", res.fail.c_str());
        return buf;
    }
    if (res.view.kind != track::ViewInfo::Kind::CBV || res.va == 0)
    {
        snprintf(buf, sizeof(buf), "%s: %s: not a CBV (kind %d)", r.name, res.how.c_str(), static_cast<int>(res.view.kind));
        return buf;
    }
    track::BufferInfo b;
    if (!track::FindBuffer(res.va, &b))
    {
        snprintf(buf, sizeof(buf), "%s: %s: VA 0x%llx (%s) outside known buffers (created before RTSky hooked?)", r.name, res.how.c_str(),
                 static_cast<unsigned long long>(res.va), (res.va & 255) == 0 ? "256-aligned" : "NOT 256-aligned");
        return buf;
    }
    snprintf(buf, sizeof(buf), "%s: %s: VA 0x%llx%s = buffer %p +%llu of %llu bytes, %s heap%s", r.name, res.how.c_str(),
             static_cast<unsigned long long>(res.va), (res.va & 255) == 0 ? "" : " (NOT 256-aligned)", static_cast<void*>(b.resource),
             static_cast<unsigned long long>(res.va - b.base), static_cast<unsigned long long>(b.size), HeapName(b.heapType),
             res.view.cbvSize != 0 ? (" (view " + std::to_string(res.view.cbvSize) + " bytes)").c_str() : "");
    return buf;
}

struct F4
{
    float x, y, z, w;
};

// [GameCB]: the sky-ambient constants as the game set them for this draw (CPU read of the upload
// buffer; the values may be mid-update, which the next line would show).
void LogGameConstants(const Resolved cb[CB_Count])
{
    F4 lg[7] = {};    // lighting_globals regs 43..49
    F4 mg14 = {};     // misc_globals reg14 (z = S)
    float ll23[3] = {}; // lighting_locals reg23 (x fade target, y flags, z AO power k)
    const bool haveLg = cb[CB_Lighting].ok && cb[CB_Lighting].va != 0 && track::ReadBufferCpu(cb[CB_Lighting].va + 43 * 16, lg, sizeof(lg), nullptr);
    const bool haveMg = cb[CB_Misc].ok && cb[CB_Misc].va != 0 && track::ReadBufferCpu(cb[CB_Misc].va + 14 * 16, &mg14, sizeof(mg14), nullptr);
    const bool haveLl = cb[CB_Locals].ok && cb[CB_Locals].va != 0 && track::ReadBufferCpu(cb[CB_Locals].va + 23 * 16, ll23, sizeof(ll23), nullptr);
    const game::EnvironmentSample env = game::Game().GetEnvironment();
    uint32_t flags = 0;
    std::memcpy(&flags, &ll23[1], 4);
    char buf[1024];
    snprintf(buf, sizeof(buf),
             "[GameCB] hour %.2f: S=%s%.4f | k=%s%.3f reg23=(x=%.4f, y=0x%X, z=%.3f) | Nat0=(%.4f,%.4f,%.4f|%.4f) Nat1=(%.4f,%.4f,%.4f|%.4f) "
             "DirAmb=(%.4f,%.4f,%.4f) packedDir=(%.3f,%.3f,%.3f) | Int0=(%.3f,%.3f,%.3f) Int1=(%.3f,%.3f,%.3f) Ext0=(%.3f,%.3f,%.3f) "
             "Ext1=(%.3f,%.3f,%.3f)%s%s%s",
             env.hours, haveMg ? "" : "?", mg14.z, haveLl ? "" : "?", ll23[2], ll23[0], flags, ll23[2], lg[0].x, lg[0].y, lg[0].z, lg[0].w,
             lg[1].x, lg[1].y, lg[1].z, lg[1].w, lg[6].x, lg[6].y, lg[6].z, lg[3].w, lg[4].w, lg[5].w, lg[2].x, lg[2].y, lg[2].z, lg[3].x,
             lg[3].y, lg[3].z, lg[4].x, lg[4].y, lg[4].z, lg[5].x, lg[5].y, lg[5].z, haveLg ? "" : " [lighting_globals not CPU-readable]",
             haveMg ? "" : " [misc_globals not CPU-readable]", haveLl ? "" : " [lighting_locals not CPU-readable]");
    LOG_INFO("%s", buf);
}

void Census(const ListState& s, bool full)
{
    track::RootLayout layout;
    const bool haveLayout = s.graphics.rootSignature != nullptr && track::GetRootLayout(s.graphics.rootSignature, &layout);
    if (full)
    {
        std::string names;
        char buf[160];
        for (int i = 1; i < static_cast<int>(track::PassId::Count); ++i)
        {
            const auto id = static_cast<track::PassId>(i);
            snprintf(buf, sizeof(buf), "%s%s %llu", names.empty() ? "" : ", ", track::PassName(id),
                     static_cast<unsigned long long>(track::PassPipelines(id)));
            names += buf;
        }
        LOG_INFO("[Names] pipelines noted %llu, with entry names %llu | %s", static_cast<unsigned long long>(track::NotedPipelines()),
                 static_cast<unsigned long long>(track::NamedPipelines()), names.c_str());
        if (haveLayout)
            LOG_INFO("[RootSig] %llu root signatures captured; PS_directional_standard binds %p: %s",
                     static_cast<unsigned long long>(track::RootSignaturesNoted()), static_cast<void*>(s.graphics.rootSignature),
                     track::DescribeLayout(layout).c_str());
        else
            LOG_WARN("[RootSig] %llu root signatures captured; PS_directional_standard binds %p, which RTSky did not capture",
                     static_cast<unsigned long long>(track::RootSignaturesNoted()), static_cast<void*>(s.graphics.rootSignature));
        LOG_INFO("[Inputs] %llu buffers tracked, shader-descriptor tracking %s, %u bound descriptor heaps",
                 static_cast<unsigned long long>(track::BuffersTracked()), track::Descriptors().TrackShaderViews() ? "on" : "OFF", s.heapCount);
    }
    if (!haveLayout)
        return;

    Resolved cb[CB_Count];
    for (int i = 0; i < CB_Count; ++i)
        cb[i] = Resolve(s, layout, kCbs[i]);
    if (full)
    {
        for (const Reg& r : kSrvs)
            LOG_INFO("[Inputs] %s", DescribeSrv(s, r, Resolve(s, layout, r)).c_str());
        for (int i = 0; i < CB_Count; ++i)
            LOG_INFO("[Inputs] %s", DescribeCb(kCbs[i], cb[i]).c_str());
    }
    LogGameConstants(cb);
}

} // namespace

void OnLightingDraw(const ListState& state)
{
    const ULONGLONG now = GetTickCount64();
    if (now < g_nextCensus.load(std::memory_order_relaxed))
        return;
    bool expected = false;
    if (!g_busy.compare_exchange_strong(expected, true))
        return;
    g_nextCensus.store(now + kGameCbInterval, std::memory_order_relaxed);
    Census(state, !g_fullDone.exchange(true));
    g_busy.store(false);
}

} // namespace rtsky::render
