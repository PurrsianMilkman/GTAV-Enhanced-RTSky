// RTSky - COM vtable slot indices, computed by the C compiler from the SDK's C interface
// definitions (see VTableIndices.c) instead of being hand-counted.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// ID3D12Device (newest interface containing the method)
extern const unsigned RTSKY_IDX_Device_CreateCommandList;
extern const unsigned RTSKY_IDX_Device_CreateShaderResourceView;
extern const unsigned RTSKY_IDX_Device_CreateRenderTargetView;
extern const unsigned RTSKY_IDX_Device_CreateDepthStencilView;
extern const unsigned RTSKY_IDX_Device_CreateCommandList1;
extern const unsigned RTSKY_IDX_Device_CopyDescriptors;
extern const unsigned RTSKY_IDX_Device_CopyDescriptorsSimple;
extern const unsigned RTSKY_IDX_Device_CreateCommandSignature;
extern const unsigned RTSKY_IDX_Device_CreateCommandQueue;
extern const unsigned RTSKY_IDX_Device_CreateGraphicsPipelineState;
extern const unsigned RTSKY_IDX_Device_CreatePipelineLibrary;
extern const unsigned RTSKY_IDX_Device_CreatePipelineState;

// ID3D12PipelineLibrary1
extern const unsigned RTSKY_IDX_Library_LoadGraphicsPipeline;
extern const unsigned RTSKY_IDX_Library_LoadPipeline;

// ID3D12GraphicsCommandList7
extern const unsigned RTSKY_IDX_CL_Close;
extern const unsigned RTSKY_IDX_CL_Reset;
extern const unsigned RTSKY_IDX_CL_DrawInstanced;
extern const unsigned RTSKY_IDX_CL_DrawIndexedInstanced;
extern const unsigned RTSKY_IDX_CL_RSSetViewports;
extern const unsigned RTSKY_IDX_CL_SetPipelineState;
extern const unsigned RTSKY_IDX_CL_ResourceBarrier;
extern const unsigned RTSKY_IDX_CL_SetDescriptorHeaps;
extern const unsigned RTSKY_IDX_CL_SetComputeRootSignature;
extern const unsigned RTSKY_IDX_CL_SetGraphicsRootSignature;
extern const unsigned RTSKY_IDX_CL_SetComputeRootDescriptorTable;
extern const unsigned RTSKY_IDX_CL_SetGraphicsRootDescriptorTable;
extern const unsigned RTSKY_IDX_CL_SetComputeRoot32BitConstant;
extern const unsigned RTSKY_IDX_CL_SetGraphicsRoot32BitConstant;
extern const unsigned RTSKY_IDX_CL_SetComputeRoot32BitConstants;
extern const unsigned RTSKY_IDX_CL_SetGraphicsRoot32BitConstants;
extern const unsigned RTSKY_IDX_CL_SetComputeRootConstantBufferView;
extern const unsigned RTSKY_IDX_CL_SetGraphicsRootConstantBufferView;
extern const unsigned RTSKY_IDX_CL_SetComputeRootShaderResourceView;
extern const unsigned RTSKY_IDX_CL_SetGraphicsRootShaderResourceView;
extern const unsigned RTSKY_IDX_CL_SetComputeRootUnorderedAccessView;
extern const unsigned RTSKY_IDX_CL_SetGraphicsRootUnorderedAccessView;
extern const unsigned RTSKY_IDX_CL_OMSetRenderTargets;
extern const unsigned RTSKY_IDX_CL_ClearDepthStencilView;
extern const unsigned RTSKY_IDX_CL_ExecuteIndirect;
extern const unsigned RTSKY_IDX_CL_BeginRenderPass;
extern const unsigned RTSKY_IDX_CL_EndRenderPass;
extern const unsigned RTSKY_IDX_CL_BuildRaytracingAccelerationStructure;
extern const unsigned RTSKY_IDX_CL_SetPipelineState1;
extern const unsigned RTSKY_IDX_CL_DispatchRays;
extern const unsigned RTSKY_IDX_CL_ExecuteBundle;
extern const unsigned RTSKY_IDX_CL_Barrier;
extern const unsigned RTSKY_IDX_CL_Count; // number of slots in ID3D12GraphicsCommandList7

// ID3D12CommandQueue
extern const unsigned RTSKY_IDX_Queue_ExecuteCommandLists;

#ifdef __cplusplus
}
#endif
