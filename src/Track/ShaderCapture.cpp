// RTSky - the game's pixel shaders, for naming passes
#include "ShaderCapture.h"

#include "../Common/Log.h"

#include <windows.h>
#include <d3dx12.h>

#include <atomic>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>

namespace rtsky::track {
namespace {

struct PipelineInfo
{
    uint64_t psHash = 0;
    PassId pass = PassId::Unknown;
    const std::string* name = nullptr;           // interned per shader (g_shaderNames), never freed
    ID3D12RootSignature* rootSignature = nullptr; // not AddRef'd, compared / looked up only
};

struct ShaderName
{
    std::string name;
    PassId pass = PassId::Unknown;
};

SRWLOCK g_lock = SRWLOCK_INIT;
// Never destroyed: pipelines can still be created during process exit, after static destructors.
std::unordered_map<const void*, PipelineInfo>* g_pipelines = new std::unordered_map<const void*, PipelineInfo>();
std::unordered_map<uint64_t, ShaderName>* g_shaderNames = new std::unordered_map<uint64_t, ShaderName>();
std::unordered_set<uint64_t>* g_written = new std::unordered_set<uint64_t>();
std::wstring* g_directory = new std::wstring();
std::atomic<uint64_t> g_noted{ 0 };
std::atomic<uint64_t> g_named{ 0 };
std::atomic<uint64_t> g_perPass[static_cast<size_t>(PassId::Count)] = {};

uint64_t Fnv1a64(const void* data, size_t size)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i)
    {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

// g_lock held.
void WriteShader(uint64_t hash, const D3D12_SHADER_BYTECODE& ps)
{
    if (g_directory->empty() || !g_written->insert(hash).second)
        return;
    CreateDirectoryW(g_directory->c_str(), nullptr);
    wchar_t name[40];
    swprintf(name, 40, L"\\ps_%016llx.dxil", static_cast<unsigned long long>(hash));
    FILE* f = _wfopen((*g_directory + name).c_str(), L"wb");
    if (f == nullptr)
    {
        RTSKY_LOG_ONCE(log::Level::Error, "Shader capture: could not write into the capture directory");
        return;
    }
    fwrite(ps.pShaderBytecode, 1, ps.BytecodeLength, f);
    fclose(f);
    if (g_written->size() == 1)
        LOG_INFO("Shader capture: writing the game's pixel shaders to RTSky_shaders");
}

struct PipelineStreamFinder : ID3DX12PipelineParserCallbacks
{
    D3D12_SHADER_BYTECODE ps = {};
    ID3D12RootSignature* rootSignature = nullptr;
    void PSCb(const D3D12_SHADER_BYTECODE& bytecode) override { ps = bytecode; }
    void RootSignatureCb(ID3D12RootSignature* rs) override { rootSignature = rs; }
};

template <typename T>
T Lookup(const void* pso, T PipelineInfo::*field, T fallback)
{
    AcquireSRWLockShared(&g_lock);
    auto it = g_pipelines->find(pso);
    const T value = it != g_pipelines->end() ? it->second.*field : fallback;
    ReleaseSRWLockShared(&g_lock);
    return value;
}

} // namespace

void SetShaderCaptureDirectory(const std::wstring& directory)
{
    AcquireSRWLockExclusive(&g_lock);
    *g_directory = directory;
    ReleaseSRWLockExclusive(&g_lock);
}

void NotePipeline(ID3D12PipelineState* pso, const D3D12_SHADER_BYTECODE& ps, ID3D12RootSignature* rootSignature)
{
    if (pso == nullptr)
        return;
    PipelineInfo info;
    info.rootSignature = rootSignature;
    const bool hasPs = ps.pShaderBytecode != nullptr && ps.BytecodeLength > 0;
    if (hasPs)
        info.psHash = Fnv1a64(ps.pShaderBytecode, ps.BytecodeLength);

    AcquireSRWLockExclusive(&g_lock);
    if (hasPs)
    {
        // Names are scanned once per distinct shader, not per pipeline.
        auto it = g_shaderNames->find(info.psHash);
        if (it == g_shaderNames->end())
        {
            ShaderName n;
            n.name = FindEntryName(ps.pShaderBytecode, ps.BytecodeLength);
            n.pass = PassIdFromEntryName(n.name);
            it = g_shaderNames->emplace(info.psHash, std::move(n)).first;
        }
        info.pass = it->second.pass;
        if (!it->second.name.empty())
            info.name = &it->second.name;
        WriteShader(info.psHash, ps);
    }
    // Keyed by pointer: a released pipeline's address can be reused, and the new one overwrites it.
    (*g_pipelines)[pso] = info;
    ReleaseSRWLockExclusive(&g_lock);

    g_noted.fetch_add(1, std::memory_order_relaxed);
    if (info.name != nullptr)
        g_named.fetch_add(1, std::memory_order_relaxed);
    g_perPass[static_cast<size_t>(info.pass)].fetch_add(1, std::memory_order_relaxed);
}

void NotePipelineStream(ID3D12PipelineState* pso, const D3D12_PIPELINE_STATE_STREAM_DESC& desc)
{
    PipelineStreamFinder finder;
    if (desc.pPipelineStateSubobjectStream != nullptr && SUCCEEDED(D3DX12ParsePipelineStream(desc, &finder)))
        NotePipeline(pso, finder.ps, finder.rootSignature);
}

uint64_t PixelShaderHash(const void* pso)
{
    return Lookup<uint64_t>(pso, &PipelineInfo::psHash, 0);
}

PassId PixelShaderPass(const void* pso)
{
    return Lookup<PassId>(pso, &PipelineInfo::pass, PassId::Unknown);
}

std::string PixelShaderName(const void* pso)
{
    const std::string* name = Lookup<const std::string*>(pso, &PipelineInfo::name, nullptr);
    return name != nullptr ? *name : std::string();
}

ID3D12RootSignature* PipelineRootSignature(const void* pso)
{
    return Lookup<ID3D12RootSignature*>(pso, &PipelineInfo::rootSignature, nullptr);
}

std::string PipelineLabel(const void* pso)
{
    AcquireSRWLockShared(&g_lock);
    auto it = g_pipelines->find(pso);
    std::string label = "?";
    if (it != g_pipelines->end())
    {
        if (it->second.name != nullptr)
        {
            label = *it->second.name;
        }
        else if (it->second.psHash != 0)
        {
            char buf[20];
            snprintf(buf, sizeof(buf), "#%016llx", static_cast<unsigned long long>(it->second.psHash));
            label = buf;
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return label;
}

uint64_t NotedPipelines()
{
    return g_noted.load(std::memory_order_relaxed);
}

uint64_t NamedPipelines()
{
    return g_named.load(std::memory_order_relaxed);
}

uint64_t PassPipelines(PassId id)
{
    return g_perPass[static_cast<size_t>(id)].load(std::memory_order_relaxed);
}

} // namespace rtsky::track
