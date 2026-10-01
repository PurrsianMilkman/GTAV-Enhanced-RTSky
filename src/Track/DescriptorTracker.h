// RTSky - CPU descriptor handle -> view information (RTV / DSV / acceleration-structure SRV)
#pragma once

#include <d3d12.h>

#include <cstdint>

namespace rtsky::track {

struct ViewInfo
{
    enum class Kind : uint8_t { None, RTV, DSV, SRV_AS };

    ID3D12Resource* resource = nullptr; // not AddRef'd; only dereferenced while the game binds it
    DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT resourceFormat = DXGI_FORMAT_UNKNOWN;
    UINT64 width = 0;
    UINT height = 0;
    UINT16 depthOrArraySize = 0;
    UINT16 mipLevels = 0;
    UINT sampleCount = 1;
    D3D12_RESOURCE_FLAGS resourceFlags = D3D12_RESOURCE_FLAG_NONE;
    D3D12_DSV_FLAGS dsvFlags = D3D12_DSV_FLAG_NONE;
    D3D12_GPU_VIRTUAL_ADDRESS asLocation = 0;
    Kind kind = Kind::None;
};

class DescriptorTracker
{
public:
    void OnCreateRTV(ID3D12Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE handle);
    void OnCreateDSV(ID3D12Resource* resource, const D3D12_DEPTH_STENCIL_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE handle);
    void OnCreateSRV(ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE handle);

    // Returns false if the handle is unknown.
    bool Lookup(D3D12_CPU_DESCRIPTOR_HANDLE handle, ViewInfo* out) const;

    // CopyDescriptors: the destination takes the source's view information unchanged (no resource
    // is dereferenced - it may be gone), or forgets what it held when the source is untracked.
    void Copy(D3D12_CPU_DESCRIPTOR_HANDLE dst, D3D12_CPU_DESCRIPTOR_HANDLE src);

    size_t Size() const;

private:
    void Store(D3D12_CPU_DESCRIPTOR_HANDLE handle, const ViewInfo& info);
    void Erase(D3D12_CPU_DESCRIPTOR_HANDLE handle);

    struct Shard;
    static constexpr size_t kShards = 16;
    Shard* ShardFor(SIZE_T ptr) const;
    Shard* m_shards[kShards] = {};

public:
    DescriptorTracker();
    ~DescriptorTracker();
    DescriptorTracker(const DescriptorTracker&) = delete;
    DescriptorTracker& operator=(const DescriptorTracker&) = delete;
};

DescriptorTracker& Descriptors();

} // namespace rtsky::track
