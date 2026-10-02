// RTSky - the game's pixel shaders, for naming passes
//
// Every graphics pipeline the game creates (CreateGraphicsPipelineState, CreatePipelineState,
// ID3D12PipelineLibrary loads) is noted with a hash of its pixel shader, the shader's entry name
// (PassNames: GTA V Enhanced keeps them, e.g. PS_directional_standard), the pass that name stands for
// and the pipeline's root signature. The analyzer arms its rules by pass name; the frame dump prints
// the names. With a capture directory set ([Detection] CaptureShaders=1), each distinct pixel shader
// is also written there once as ps_<hash>.dxil, ready for `dxc -dumpbin`.
#pragma once

#include "PassNames.h"

#include <d3d12.h>

#include <cstdint>
#include <string>

namespace rtsky::track {

// Empty = no files (the default). Applied to pipelines created from now on.
void SetShaderCaptureDirectory(const std::wstring& directory);

void NotePipeline(ID3D12PipelineState* pso, const D3D12_SHADER_BYTECODE& ps, ID3D12RootSignature* rootSignature);
void NotePipelineStream(ID3D12PipelineState* pso, const D3D12_PIPELINE_STATE_STREAM_DESC& desc);

// Lookups by pipeline (0 / Unknown / empty / nullptr when the pipeline was created before the hooks).
uint64_t PixelShaderHash(const void* pso);
PassId PixelShaderPass(const void* pso);
std::string PixelShaderName(const void* pso);
ID3D12RootSignature* PipelineRootSignature(const void* pso);

// For the frame dump: the entry name, else "#<hash>", else "?".
std::string PipelineLabel(const void* pso);

// Statistics for the log: pipelines noted, pipelines whose pixel shader has an entry name, and
// pipelines per named pass.
uint64_t NotedPipelines();
uint64_t NamedPipelines();
uint64_t PassPipelines(PassId id);

} // namespace rtsky::track
