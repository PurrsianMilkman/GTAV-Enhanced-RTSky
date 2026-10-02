// RTSky - layouts of the game's root signatures
#include "RootSignatureTracker.h"

#include "../Common/Log.h"

#include <windows.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdio>
#include <memory>
#include <unordered_map>

namespace rtsky::track {
namespace {

SRWLOCK g_lock = SRWLOCK_INIT;
// Never destroyed (hooks can run during process exit). Keyed by pointer: a released root signature's
// address can be reused, and the new one overwrites it.
std::unordered_map<const void*, std::shared_ptr<const RootLayout>>* g_layouts =
    new std::unordered_map<const void*, std::shared_ptr<const RootLayout>>();
std::atomic<uint64_t> g_noted{ 0 };

template <typename Range>
void AddRanges(RootParam& p, const Range* ranges, UINT count)
{
    uint32_t next = 0;
    for (UINT i = 0; i < count; ++i)
    {
        RootRange r;
        r.type = ranges[i].RangeType;
        r.baseRegister = ranges[i].BaseShaderRegister;
        r.space = ranges[i].RegisterSpace;
        r.count = ranges[i].NumDescriptors;
        r.offsetInTable = ranges[i].OffsetInDescriptorsFromTableStart == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
            ? next
            : ranges[i].OffsetInDescriptorsFromTableStart;
        next = r.count == UINT_MAX ? UINT_MAX : r.offsetInTable + r.count;
        p.ranges.push_back(r);
    }
}

template <typename Param>
RootParam ParamFrom(const Param& src)
{
    RootParam p;
    p.type = src.ParameterType;
    p.visibility = src.ShaderVisibility;
    switch (src.ParameterType)
    {
    case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
        AddRanges(p, src.DescriptorTable.pDescriptorRanges, src.DescriptorTable.NumDescriptorRanges);
        break;
    case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
        p.reg = src.Constants.ShaderRegister;
        p.space = src.Constants.RegisterSpace;
        p.num32 = src.Constants.Num32BitValues;
        break;
    default:
        p.reg = src.Descriptor.ShaderRegister;
        p.space = src.Descriptor.RegisterSpace;
        break;
    }
    return p;
}

D3D12_ROOT_PARAMETER_TYPE RootTypeFor(D3D12_DESCRIPTOR_RANGE_TYPE kind)
{
    switch (kind)
    {
    case D3D12_DESCRIPTOR_RANGE_TYPE_CBV: return D3D12_ROOT_PARAMETER_TYPE_CBV;
    case D3D12_DESCRIPTOR_RANGE_TYPE_UAV: return D3D12_ROOT_PARAMETER_TYPE_UAV;
    default: return D3D12_ROOT_PARAMETER_TYPE_SRV;
    }
}

const char* KindLetter(D3D12_DESCRIPTOR_RANGE_TYPE t)
{
    switch (t)
    {
    case D3D12_DESCRIPTOR_RANGE_TYPE_CBV: return "b";
    case D3D12_DESCRIPTOR_RANGE_TYPE_UAV: return "u";
    case D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER: return "s";
    default: return "t";
    }
}

} // namespace

RootLayout LayoutFromDesc(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC& desc)
{
    RootLayout layout;
    if (desc.Version == D3D_ROOT_SIGNATURE_VERSION_1_0)
    {
        for (UINT i = 0; i < desc.Desc_1_0.NumParameters; ++i)
            layout.params.push_back(ParamFrom(desc.Desc_1_0.pParameters[i]));
    }
    else
    {
        for (UINT i = 0; i < desc.Desc_1_1.NumParameters; ++i)
            layout.params.push_back(ParamFrom(desc.Desc_1_1.pParameters[i]));
    }
    return layout;
}

bool ResolveRegister(const RootLayout& layout, D3D12_DESCRIPTOR_RANGE_TYPE kind, uint32_t reg, uint32_t space, RootLocation* out)
{
    for (size_t i = 0; i < layout.params.size(); ++i)
    {
        const RootParam& p = layout.params[i];
        if (p.visibility != D3D12_SHADER_VISIBILITY_ALL && p.visibility != D3D12_SHADER_VISIBILITY_PIXEL)
            continue;
        if (p.type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
        {
            for (const RootRange& r : p.ranges)
            {
                if (r.type != kind || r.space != space || reg < r.baseRegister)
                    continue;
                if (r.count != UINT_MAX && reg >= r.baseRegister + r.count)
                    continue;
                out->param = static_cast<int32_t>(i);
                out->descriptorOffset = static_cast<int64_t>(r.offsetInTable) + (reg - r.baseRegister);
                out->type = p.type;
                return true;
            }
        }
        else if (p.type == RootTypeFor(kind) && p.reg == reg && p.space == space)
        {
            out->param = static_cast<int32_t>(i);
            out->descriptorOffset = -1;
            out->type = p.type;
            return true;
        }
    }
    return false;
}

void NoteRootSignature(ID3D12RootSignature* rs, const void* blob, size_t size)
{
    if (rs == nullptr || blob == nullptr || size == 0)
        return;
    using PFN = HRESULT(WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);
    static PFN create = [] {
        HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
        return d3d12 != nullptr
            ? reinterpret_cast<PFN>(reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12CreateVersionedRootSignatureDeserializer")))
            : nullptr;
    }();
    if (create == nullptr)
    {
        RTSKY_LOG_ONCE(log::Level::Warning, "D3D12CreateVersionedRootSignatureDeserializer unavailable: root signatures are not named");
        return;
    }
    Microsoft::WRL::ComPtr<ID3D12VersionedRootSignatureDeserializer> deserializer;
    if (FAILED(create(blob, size, IID_PPV_ARGS(&deserializer))))
        return;
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc = nullptr;
    if (FAILED(deserializer->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &desc)) || desc == nullptr)
        return;
    auto layout = std::make_shared<const RootLayout>(LayoutFromDesc(*desc));
    AcquireSRWLockExclusive(&g_lock);
    (*g_layouts)[rs] = std::move(layout);
    ReleaseSRWLockExclusive(&g_lock);
    g_noted.fetch_add(1, std::memory_order_relaxed);
}

bool GetRootLayout(const ID3D12RootSignature* rs, RootLayout* out)
{
    AcquireSRWLockShared(&g_lock);
    auto it = g_layouts->find(rs);
    std::shared_ptr<const RootLayout> layout = it != g_layouts->end() ? it->second : nullptr;
    ReleaseSRWLockShared(&g_lock);
    if (!layout)
        return false;
    *out = *layout;
    return true;
}

uint64_t RootSignaturesNoted()
{
    return g_noted.load(std::memory_order_relaxed);
}

std::string DescribeLayout(const RootLayout& layout)
{
    std::string s;
    char buf[96];
    for (size_t i = 0; i < layout.params.size(); ++i)
    {
        const RootParam& p = layout.params[i];
        if (i > 0)
            s += " | ";
        switch (p.type)
        {
        case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
            snprintf(buf, sizeof(buf), "p%zu table", i);
            s += buf;
            for (const RootRange& r : p.ranges)
            {
                if (r.count == UINT_MAX)
                    snprintf(buf, sizeof(buf), " %s%u-.. s%u @%u", KindLetter(r.type), r.baseRegister, r.space, r.offsetInTable);
                else
                    snprintf(buf, sizeof(buf), " %s%u-%u s%u @%u", KindLetter(r.type), r.baseRegister, r.baseRegister + r.count - 1, r.space,
                             r.offsetInTable);
                s += buf;
            }
            break;
        case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
            snprintf(buf, sizeof(buf), "p%zu consts b%u s%u x%u", i, p.reg, p.space, p.num32);
            s += buf;
            break;
        default:
            snprintf(buf, sizeof(buf), "p%zu %s %s%u s%u", i,
                     p.type == D3D12_ROOT_PARAMETER_TYPE_CBV ? "CBV" : p.type == D3D12_ROOT_PARAMETER_TYPE_SRV ? "SRV" : "UAV",
                     p.type == D3D12_ROOT_PARAMETER_TYPE_CBV ? "b" : p.type == D3D12_ROOT_PARAMETER_TYPE_SRV ? "t" : "u", p.reg, p.space);
            s += buf;
            break;
        }
        if (p.visibility != D3D12_SHADER_VISIBILITY_ALL)
        {
            snprintf(buf, sizeof(buf), " (vis %d)", static_cast<int>(p.visibility));
            s += buf;
        }
    }
    return s;
}

} // namespace rtsky::track
