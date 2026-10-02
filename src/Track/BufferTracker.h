// RTSky - GPU virtual address -> buffer
//
// Every buffer the game creates (committed or placed) is recorded with its GPU virtual address range
// and heap type, and forgotten when it is destroyed (private-data destruction watch). A root CBV
// address (for example the game's lighting constants at PS_directional_standard's draw) can so be
// traced to its buffer: whether it is an UPLOAD heap that the CPU can read, or a DEFAULT heap.
#pragma once

#include <d3d12.h>

#include <cstdint>

namespace rtsky::track {

struct BufferInfo
{
    ID3D12Resource* resource = nullptr; // not AddRef'd: only used while the game still binds the address
    D3D12_GPU_VIRTUAL_ADDRESS base = 0;
    UINT64 size = 0;
    D3D12_HEAP_TYPE heapType = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_CPU_PAGE_PROPERTY cpuPage = D3D12_CPU_PAGE_PROPERTY_UNKNOWN; // custom heaps
};

// From the resource-creation hooks (after the call succeeded); textures are ignored.
void NoteBuffer(ID3D12Resource* resource, D3D12_HEAP_TYPE heapType, D3D12_CPU_PAGE_PROPERTY cpuPage);

bool FindBuffer(D3D12_GPU_VIRTUAL_ADDRESS va, BufferInfo* out);

uint64_t BuffersTracked();

// Copies `size` bytes at `va` into `out` when the buffer behind it is CPU-readable (UPLOAD heap or a
// write-back / write-combined custom heap). Diagnostics only: the data may be mid-update.
bool ReadBufferCpu(D3D12_GPU_VIRTUAL_ADDRESS va, void* out, size_t size, BufferInfo* info);

} // namespace rtsky::track
