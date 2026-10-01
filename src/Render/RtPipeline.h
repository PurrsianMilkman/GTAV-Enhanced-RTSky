// RTSky - DXR ray tracing pipeline (state object + shader tables)
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

namespace rtsky::render {

class RtPipeline
{
public:
    // dxil: RTSkyLib.hlsl compiled as a library. allowOpacityMicromaps requires raytracing tier 1.2.
    bool Create(ID3D12Device5* device, ID3D12RootSignature* globalRootSignature, const void* dxil, size_t dxilSize,
                bool allowOpacityMicromaps);
    bool IsValid() const { return m_stateObject != nullptr; }
    bool AllowsOpacityMicromaps() const { return m_allowOmm; }

    // Sets the state object and dispatches; the caller has set the global root signature and all
    // root arguments with SetComputeRoot*.
    void Dispatch(ID3D12GraphicsCommandList4* list, UINT width, UINT height) const;

    ID3D12StateObject* StateObject() const { return m_stateObject.Get(); }
    ID3D12Resource* ShaderTable() const { return m_shaderTable.Get(); }

private:
    Microsoft::WRL::ComPtr<ID3D12StateObject> m_stateObject;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_shaderTable;
    D3D12_DISPATCH_RAYS_DESC m_dispatchDesc = {};
    bool m_allowOmm = false;
};

} // namespace rtsky::render
