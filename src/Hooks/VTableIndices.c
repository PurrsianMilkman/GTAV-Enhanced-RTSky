/* RTSky - COM vtable slot indices.
 * Compiled as C so that the SDK exposes the C interface definitions (<Interface>Vtbl structs); the
 * slot index of a method is its byte offset in the Vtbl struct divided by the pointer size. */
#define COBJMACROS
#include <windows.h>
#include <d3d12.h>
#include <stddef.h>

#include "VTableIndices.h"

#define RTSKY_SLOT(iface, method) ((unsigned)(offsetof(iface##Vtbl, method) / sizeof(void*)))

const unsigned RTSKY_IDX_Device_CreateCommandList = RTSKY_SLOT(ID3D12Device5, CreateCommandList);
const unsigned RTSKY_IDX_Device_CreateShaderResourceView = RTSKY_SLOT(ID3D12Device5, CreateShaderResourceView);
const unsigned RTSKY_IDX_Device_CreateRenderTargetView = RTSKY_SLOT(ID3D12Device5, CreateRenderTargetView);
const unsigned RTSKY_IDX_Device_CreateDepthStencilView = RTSKY_SLOT(ID3D12Device5, CreateDepthStencilView);
const unsigned RTSKY_IDX_Device_CreateCommandList1 = RTSKY_SLOT(ID3D12Device5, CreateCommandList1);
const unsigned RTSKY_IDX_Device_CopyDescriptors = RTSKY_SLOT(ID3D12Device5, CopyDescriptors);
const unsigned RTSKY_IDX_Device_CopyDescriptorsSimple = RTSKY_SLOT(ID3D12Device5, CopyDescriptorsSimple);
const unsigned RTSKY_IDX_Device_CreateCommandSignature = RTSKY_SLOT(ID3D12Device5, CreateCommandSignature);
const unsigned RTSKY_IDX_Device_CreateCommandQueue = RTSKY_SLOT(ID3D12Device5, CreateCommandQueue);
const unsigned RTSKY_IDX_Device_CreateGraphicsPipelineState = RTSKY_SLOT(ID3D12Device5, CreateGraphicsPipelineState);
const unsigned RTSKY_IDX_Device_CreatePipelineLibrary = RTSKY_SLOT(ID3D12Device5, CreatePipelineLibrary);
const unsigned RTSKY_IDX_Device_CreatePipelineState = RTSKY_SLOT(ID3D12Device5, CreatePipelineState);

const unsigned RTSKY_IDX_Library_LoadGraphicsPipeline = RTSKY_SLOT(ID3D12PipelineLibrary1, LoadGraphicsPipeline);
const unsigned RTSKY_IDX_Library_LoadPipeline = RTSKY_SLOT(ID3D12PipelineLibrary1, LoadPipeline);

const unsigned RTSKY_IDX_CL_Close = RTSKY_SLOT(ID3D12GraphicsCommandList7, Close);
const unsigned RTSKY_IDX_CL_Reset = RTSKY_SLOT(ID3D12GraphicsCommandList7, Reset);
const unsigned RTSKY_IDX_CL_DrawInstanced = RTSKY_SLOT(ID3D12GraphicsCommandList7, DrawInstanced);
const unsigned RTSKY_IDX_CL_DrawIndexedInstanced = RTSKY_SLOT(ID3D12GraphicsCommandList7, DrawIndexedInstanced);
const unsigned RTSKY_IDX_CL_RSSetViewports = RTSKY_SLOT(ID3D12GraphicsCommandList7, RSSetViewports);
const unsigned RTSKY_IDX_CL_SetPipelineState = RTSKY_SLOT(ID3D12GraphicsCommandList7, SetPipelineState);
const unsigned RTSKY_IDX_CL_ResourceBarrier = RTSKY_SLOT(ID3D12GraphicsCommandList7, ResourceBarrier);
const unsigned RTSKY_IDX_CL_SetDescriptorHeaps = RTSKY_SLOT(ID3D12GraphicsCommandList7, SetDescriptorHeaps);
const unsigned RTSKY_IDX_CL_SetComputeRootSignature = RTSKY_SLOT(ID3D12GraphicsCommandList7, SetComputeRootSignature);
const unsigned RTSKY_IDX_CL_SetGraphicsRootSignature = RTSKY_SLOT(ID3D12GraphicsCommandList7, SetGraphicsRootSignature);
const unsigned RTSKY_IDX_CL_SetComputeRootDescriptorTable = RTSKY_SLOT(ID3D12GraphicsCommandList7, SetComputeRootDescriptorTable);
const unsigned RTSKY_IDX_CL_SetGraphicsRootDescriptorTable = RTSKY_SLOT(ID3D12GraphicsCommandList7, SetGraphicsRootDescriptorTable);
const unsigned RTSKY_IDX_CL_SetComputeRoot32BitConstant = RTSKY_SLOT(ID3D12GraphicsCommandList7, SetComputeRoot32BitConstant);
const unsigned RTSKY_IDX_CL_SetGraphicsRoot32BitConstant = RTSKY_SLOT(ID3D12GraphicsCommandList7, SetGraphicsRoot32BitConstant);
const unsigned RTSKY_IDX_CL_SetComputeRoot32BitConstants = RTSKY_SLOT(ID3D12GraphicsCommandList7, SetComputeRoot32BitConstants);
const unsigned RTSKY_IDX_CL_SetGraphicsRoot32BitConstants = RTSKY_SLOT(ID3D12GraphicsCommandList7, SetGraphicsRoot32BitConstants);
const unsigned RTSKY_IDX_CL_SetComputeRootConstantBufferView = RTSKY_SLOT(ID3D12GraphicsCommandList7, SetComputeRootConstantBufferView);
const unsigned RTSKY_IDX_CL_SetGraphicsRootConstantBufferView = RTSKY_SLOT(ID3D12GraphicsCommandList7, SetGraphicsRootConstantBufferView);
const unsigned RTSKY_IDX_CL_SetComputeRootShaderResourceView = RTSKY_SLOT(ID3D12GraphicsCommandList7, SetComputeRootShaderResourceView);
const unsigned RTSKY_IDX_CL_SetGraphicsRootShaderResourceView = RTSKY_SLOT(ID3D12GraphicsCommandList7, SetGraphicsRootShaderResourceView);
const unsigned RTSKY_IDX_CL_SetComputeRootUnorderedAccessView = RTSKY_SLOT(ID3D12GraphicsCommandList7, SetComputeRootUnorderedAccessView);
const unsigned RTSKY_IDX_CL_SetGraphicsRootUnorderedAccessView = RTSKY_SLOT(ID3D12GraphicsCommandList7, SetGraphicsRootUnorderedAccessView);
const unsigned RTSKY_IDX_CL_OMSetRenderTargets = RTSKY_SLOT(ID3D12GraphicsCommandList7, OMSetRenderTargets);
const unsigned RTSKY_IDX_CL_ClearDepthStencilView = RTSKY_SLOT(ID3D12GraphicsCommandList7, ClearDepthStencilView);
const unsigned RTSKY_IDX_CL_ExecuteIndirect = RTSKY_SLOT(ID3D12GraphicsCommandList7, ExecuteIndirect);
const unsigned RTSKY_IDX_CL_BeginRenderPass = RTSKY_SLOT(ID3D12GraphicsCommandList7, BeginRenderPass);
const unsigned RTSKY_IDX_CL_EndRenderPass = RTSKY_SLOT(ID3D12GraphicsCommandList7, EndRenderPass);
const unsigned RTSKY_IDX_CL_BuildRaytracingAccelerationStructure = RTSKY_SLOT(ID3D12GraphicsCommandList7, BuildRaytracingAccelerationStructure);
const unsigned RTSKY_IDX_CL_SetPipelineState1 = RTSKY_SLOT(ID3D12GraphicsCommandList7, SetPipelineState1);
const unsigned RTSKY_IDX_CL_DispatchRays = RTSKY_SLOT(ID3D12GraphicsCommandList7, DispatchRays);
const unsigned RTSKY_IDX_CL_ExecuteBundle = RTSKY_SLOT(ID3D12GraphicsCommandList7, ExecuteBundle);
const unsigned RTSKY_IDX_CL_Barrier = RTSKY_SLOT(ID3D12GraphicsCommandList7, Barrier);
const unsigned RTSKY_IDX_CL_Count = (unsigned)(sizeof(ID3D12GraphicsCommandList7Vtbl) / sizeof(void*));

const unsigned RTSKY_IDX_Queue_ExecuteCommandLists = RTSKY_SLOT(ID3D12CommandQueue, ExecuteCommandLists);

/* Guard against an outdated SDK: these reference values come from DirectX-Headers v1.619.5 and the
 * Windows SDK 10.0.22621+. COM vtable layouts are ABI and never change once published. */
_Static_assert(RTSKY_SLOT(ID3D12GraphicsCommandList7, OMSetRenderTargets) == 46, "unexpected vtable layout");
_Static_assert(RTSKY_SLOT(ID3D12GraphicsCommandList7, BuildRaytracingAccelerationStructure) == 72, "unexpected vtable layout");
_Static_assert(RTSKY_SLOT(ID3D12GraphicsCommandList7, DispatchRays) == 76, "unexpected vtable layout");
_Static_assert(RTSKY_SLOT(ID3D12GraphicsCommandList7, Barrier) == 80, "unexpected vtable layout");
_Static_assert(RTSKY_SLOT(ID3D12CommandQueue, ExecuteCommandLists) == 10, "unexpected vtable layout");
_Static_assert(RTSKY_SLOT(ID3D12Device5, CreateRenderTargetView) == 20, "unexpected vtable layout");
_Static_assert(RTSKY_SLOT(ID3D12Device5, CopyDescriptorsSimple) == 24, "unexpected vtable layout");
_Static_assert(RTSKY_SLOT(ID3D12Device5, CreateCommandSignature) == 41, "unexpected vtable layout");
_Static_assert(RTSKY_SLOT(ID3D12Device5, CreateCommandQueue) == 8, "unexpected vtable layout");
_Static_assert(RTSKY_SLOT(ID3D12Device5, CreateGraphicsPipelineState) == 10, "unexpected vtable layout");
