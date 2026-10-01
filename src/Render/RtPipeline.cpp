// RTSky - DXR ray tracing pipeline (state object + shader tables)
#include "RtPipeline.h"

#include "../Common/Log.h"

#include <d3dx12.h>

#include <cstring>

namespace rtsky::render {

using Microsoft::WRL::ComPtr;

static constexpr wchar_t kRayGen[] = L"RayGen";
static constexpr wchar_t kMiss[] = L"Miss";
static constexpr wchar_t kClosestHit[] = L"ClosestHit";
static constexpr wchar_t kAnyHit[] = L"AnyHit";
static constexpr wchar_t kHitGroup[] = L"RTSkyHitGroup";

bool RtPipeline::Create(ID3D12Device5* device, ID3D12RootSignature* globalRootSignature, const void* dxil, size_t dxilSize,
                        bool allowOpacityMicromaps)
{
    CD3DX12_STATE_OBJECT_DESC desc(D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE);

    auto* lib = desc.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();
    D3D12_SHADER_BYTECODE bytecode = { dxil, dxilSize };
    lib->SetDXILLibrary(&bytecode);
    lib->DefineExport(kRayGen);
    lib->DefineExport(kMiss);
    lib->DefineExport(kClosestHit);
    lib->DefineExport(kAnyHit);

    auto* hitGroup = desc.CreateSubobject<CD3DX12_HIT_GROUP_SUBOBJECT>();
    hitGroup->SetHitGroupExport(kHitGroup);
    hitGroup->SetHitGroupType(D3D12_HIT_GROUP_TYPE_TRIANGLES);
    hitGroup->SetClosestHitShaderImport(kClosestHit);
    hitGroup->SetAnyHitShaderImport(kAnyHit);

    auto* shaderConfig = desc.CreateSubobject<CD3DX12_RAYTRACING_SHADER_CONFIG_SUBOBJECT>();
    shaderConfig->Config(sizeof(float) /* payload */, 2 * sizeof(float) /* triangle barycentrics */);

    auto* rootSig = desc.CreateSubobject<CD3DX12_GLOBAL_ROOT_SIGNATURE_SUBOBJECT>();
    rootSig->SetRootSignature(globalRootSignature);

    if (allowOpacityMicromaps)
    {
        // CONFIG1 (tier 1.1+); OMM traversal must be enabled explicitly or it is undefined behaviour.
        auto* config1 = desc.CreateSubobject<CD3DX12_RAYTRACING_PIPELINE_CONFIG1_SUBOBJECT>();
        config1->Config(1, D3D12_RAYTRACING_PIPELINE_FLAG_ALLOW_OPACITY_MICROMAPS);
    }
    else
    {
        auto* config = desc.CreateSubobject<CD3DX12_RAYTRACING_PIPELINE_CONFIG_SUBOBJECT>();
        config->Config(1); // raygen traces, nothing recurses
    }

    HRESULT hr = device->CreateStateObject(desc, IID_PPV_ARGS(&m_stateObject));
    if (FAILED(hr))
    {
        LOG_ERROR("CreateStateObject failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }
    m_stateObject->SetName(L"RTSky ray tracing pipeline");
    m_allowOmm = allowOpacityMicromaps;

    ComPtr<ID3D12StateObjectProperties> props;
    if (FAILED(m_stateObject.As(&props)))
        return false;
    const void* idRayGen = props->GetShaderIdentifier(kRayGen);
    const void* idMiss = props->GetShaderIdentifier(kMiss);
    const void* idHitGroup = props->GetShaderIdentifier(kHitGroup);
    if (idRayGen == nullptr || idMiss == nullptr || idHitGroup == nullptr)
    {
        LOG_ERROR("Shader identifiers not found in the state object");
        m_stateObject.Reset();
        return false;
    }

    // Layout: raygen @0, miss @64, hit group @128 (table starts are 64-byte aligned, records are the
    // 32-byte identifiers with no local root arguments).
    constexpr UINT64 kTableAlign = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;
    constexpr UINT64 kIdSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
    constexpr UINT64 kRecordSize = (kIdSize + D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT - 1) & ~UINT64(D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT - 1);
    constexpr UINT64 kRayGenOffset = 0;
    constexpr UINT64 kMissOffset = kTableAlign;
    constexpr UINT64 kHitOffset = 2 * kTableAlign;
    constexpr UINT64 kTableSize = 3 * kTableAlign;

    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = kTableSize;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                         IID_PPV_ARGS(&m_shaderTable));
    if (FAILED(hr))
    {
        LOG_ERROR("Shader table allocation failed: 0x%08X", static_cast<unsigned>(hr));
        m_stateObject.Reset();
        return false;
    }
    m_shaderTable->SetName(L"RTSky shader table");

    uint8_t* mapped = nullptr;
    D3D12_RANGE noRead = { 0, 0 };
    if (FAILED(m_shaderTable->Map(0, &noRead, reinterpret_cast<void**>(&mapped))))
    {
        m_stateObject.Reset();
        return false;
    }
    std::memset(mapped, 0, kTableSize);
    std::memcpy(mapped + kRayGenOffset, idRayGen, kIdSize);
    std::memcpy(mapped + kMissOffset, idMiss, kIdSize);
    std::memcpy(mapped + kHitOffset, idHitGroup, kIdSize);
    m_shaderTable->Unmap(0, nullptr);

    const D3D12_GPU_VIRTUAL_ADDRESS base = m_shaderTable->GetGPUVirtualAddress();
    m_dispatchDesc = {};
    m_dispatchDesc.RayGenerationShaderRecord.StartAddress = base + kRayGenOffset;
    m_dispatchDesc.RayGenerationShaderRecord.SizeInBytes = kRecordSize;
    m_dispatchDesc.MissShaderTable.StartAddress = base + kMissOffset;
    m_dispatchDesc.MissShaderTable.SizeInBytes = kRecordSize;
    m_dispatchDesc.MissShaderTable.StrideInBytes = kRecordSize;
    // Stride 0: every hit-group index (the game's InstanceContributionToHitGroupIndex values included)
    // resolves to this single record (DXR spec, "Shader record stride").
    m_dispatchDesc.HitGroupTable.StartAddress = base + kHitOffset;
    m_dispatchDesc.HitGroupTable.SizeInBytes = kRecordSize;
    m_dispatchDesc.HitGroupTable.StrideInBytes = 0;
    m_dispatchDesc.Depth = 1;

    LOG_INFO("DXR pipeline created%s", allowOpacityMicromaps ? " (opacity micromaps allowed)" : "");
    return true;
}

void RtPipeline::Dispatch(ID3D12GraphicsCommandList4* list, UINT width, UINT height) const
{
    D3D12_DISPATCH_RAYS_DESC d = m_dispatchDesc;
    d.Width = width;
    d.Height = height;
    list->SetPipelineState1(m_stateObject.Get());
    list->DispatchRays(&d);
}

} // namespace rtsky::render
