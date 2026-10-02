// RTSky - the game's pixel shaders, for naming passes in the frame dump
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

SRWLOCK g_lock = SRWLOCK_INIT;
// Never destroyed: pipelines can still be created during process exit, after static destructors.
std::unordered_map<const void*, uint64_t>* g_psoHash = new std::unordered_map<const void*, uint64_t>();
std::unordered_set<uint64_t>* g_written = new std::unordered_set<uint64_t>();
std::wstring* g_directory = new std::wstring();
std::atomic<uint64_t> g_noted{ 0 };

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

struct PixelShaderFinder : ID3DX12PipelineParserCallbacks
{
    D3D12_SHADER_BYTECODE ps = {};
    void PSCb(const D3D12_SHADER_BYTECODE& bytecode) override { ps = bytecode; }
};

} // namespace

void SetShaderCaptureDirectory(const std::wstring& directory)
{
    AcquireSRWLockExclusive(&g_lock);
    *g_directory = directory;
    ReleaseSRWLockExclusive(&g_lock);
}

void NotePipeline(ID3D12PipelineState* pso, const D3D12_SHADER_BYTECODE& ps)
{
    if (pso == nullptr || ps.pShaderBytecode == nullptr || ps.BytecodeLength == 0)
        return;
    const uint64_t hash = Fnv1a64(ps.pShaderBytecode, ps.BytecodeLength);
    AcquireSRWLockExclusive(&g_lock);
    // Keyed by pointer: a released pipeline's address can be reused, and the new one overwrites it.
    (*g_psoHash)[pso] = hash;
    WriteShader(hash, ps);
    ReleaseSRWLockExclusive(&g_lock);
    g_noted.fetch_add(1, std::memory_order_relaxed);
}

void NotePipelineStream(ID3D12PipelineState* pso, const D3D12_PIPELINE_STATE_STREAM_DESC& desc)
{
    PixelShaderFinder finder;
    if (desc.pPipelineStateSubobjectStream != nullptr && SUCCEEDED(D3DX12ParsePipelineStream(desc, &finder)))
        NotePipeline(pso, finder.ps);
}

uint64_t PixelShaderHash(const void* pso)
{
    AcquireSRWLockShared(&g_lock);
    auto it = g_psoHash->find(pso);
    const uint64_t hash = it != g_psoHash->end() ? it->second : 0;
    ReleaseSRWLockShared(&g_lock);
    return hash;
}

uint64_t NotedPipelines()
{
    return g_noted.load(std::memory_order_relaxed);
}

} // namespace rtsky::track
