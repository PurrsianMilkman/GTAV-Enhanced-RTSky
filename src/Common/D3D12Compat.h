// RTSky - portability helpers for D3D12 methods that return structs.
// MSVC returns them by value; DirectX-Headers exposes them to other compilers on Windows (MinGW)
// as methods taking a hidden return pointer, which is the actual COM ABI.
#pragma once

#include <d3d12.h>

// MSVC gets interface IDs from __declspec(uuid); MinGW needs DirectX-Headers' IID table so that
// __uuidof / IID_PPV_ARGS work for the newer D3D12 interfaces.
#if !defined(_MSC_VER) && defined(__MINGW32__)
#include <dxguids/dxguids.h>
#endif

namespace rtsky {

inline D3D12_RESOURCE_DESC ResourceDesc(ID3D12Resource* resource)
{
#if defined(_MSC_VER) || !defined(_WIN32)
    return resource->GetDesc();
#else
    D3D12_RESOURCE_DESC desc;
    resource->GetDesc(&desc);
    return desc;
#endif
}

inline D3D12_HEAP_DESC HeapDesc(ID3D12Heap* heap)
{
#if defined(_MSC_VER) || !defined(_WIN32)
    return heap->GetDesc();
#else
    D3D12_HEAP_DESC desc;
    heap->GetDesc(&desc);
    return desc;
#endif
}

inline D3D12_DESCRIPTOR_HEAP_DESC DescriptorHeapDesc(ID3D12DescriptorHeap* heap)
{
#if defined(_MSC_VER) || !defined(_WIN32)
    return heap->GetDesc();
#else
    D3D12_DESCRIPTOR_HEAP_DESC desc;
    heap->GetDesc(&desc);
    return desc;
#endif
}

inline D3D12_CPU_DESCRIPTOR_HANDLE HeapCpuStart(ID3D12DescriptorHeap* heap)
{
#if defined(_MSC_VER) || !defined(_WIN32)
    return heap->GetCPUDescriptorHandleForHeapStart();
#else
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    heap->GetCPUDescriptorHandleForHeapStart(&h);
    return h;
#endif
}

inline D3D12_GPU_DESCRIPTOR_HANDLE HeapGpuStart(ID3D12DescriptorHeap* heap)
{
#if defined(_MSC_VER) || !defined(_WIN32)
    return heap->GetGPUDescriptorHandleForHeapStart();
#else
    D3D12_GPU_DESCRIPTOR_HANDLE h;
    heap->GetGPUDescriptorHandleForHeapStart(&h);
    return h;
#endif
}

} // namespace rtsky
