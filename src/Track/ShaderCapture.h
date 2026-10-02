// RTSky - the game's pixel shaders, for naming passes in the frame dump
//
// Every graphics pipeline the game creates (CreateGraphicsPipelineState, CreatePipelineState,
// ID3D12PipelineLibrary loads) is noted with a hash of its pixel shader. The frame dump prints the
// hashes of the pipelines each render-target binding drew with, so a pass can be identified by its
// shader rather than guessed from its render-target formats. With a capture directory set
// ([Detection] CaptureShaders=1), each distinct pixel shader is also written there once as
// ps_<hash>.dxil, ready for `dxc -dumpbin`.
#pragma once

#include <d3d12.h>

#include <cstdint>
#include <string>

namespace rtsky::track {

// Empty = hashes only (the default). Applied to pipelines created from now on.
void SetShaderCaptureDirectory(const std::wstring& directory);

void NotePipeline(ID3D12PipelineState* pso, const D3D12_SHADER_BYTECODE& ps);
void NotePipelineStream(ID3D12PipelineState* pso, const D3D12_PIPELINE_STATE_STREAM_DESC& desc);

// Hash of the pipeline's pixel shader; 0 = unknown (created before the hooks, or no pixel shader).
uint64_t PixelShaderHash(const void* pso);

// Pipelines noted so far (statistics for the log).
uint64_t NotedPipelines();

} // namespace rtsky::track
