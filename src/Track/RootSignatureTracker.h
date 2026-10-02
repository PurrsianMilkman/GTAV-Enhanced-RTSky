// RTSky - layouts of the game's root signatures
//
// CreateRootSignature is hooked and each blob deserialized (D3D12CreateVersionedRootSignatureDeserializer),
// so that a shader register (for example cb5 or t24,space1 of PS_directional_standard) can be mapped
// to the root parameter that carries it: a root descriptor (CBV / SRV / UAV address) or a descriptor
// table plus the offset of the descriptor inside it. Used to name a pass's inputs at its draw.
#pragma once

#include <d3d12.h>

#include <cstdint>
#include <string>
#include <vector>

namespace rtsky::track {

struct RootRange
{
    D3D12_DESCRIPTOR_RANGE_TYPE type = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    uint32_t baseRegister = 0;
    uint32_t space = 0;
    uint32_t count = 0;           // UINT_MAX = unbounded
    uint32_t offsetInTable = 0;   // descriptors from the table start (APPEND resolved)
};

struct RootParam
{
    D3D12_ROOT_PARAMETER_TYPE type = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    D3D12_SHADER_VISIBILITY visibility = D3D12_SHADER_VISIBILITY_ALL;
    uint32_t reg = 0;    // root CBV / SRV / UAV / constants
    uint32_t space = 0;
    uint32_t num32 = 0;  // root constants
    std::vector<RootRange> ranges; // descriptor table
};

struct RootLayout
{
    std::vector<RootParam> params;
};

// Where a register lives: root parameter `param`, and `descriptorOffset` descriptors into its table
// (-1 for a root descriptor).
struct RootLocation
{
    int32_t param = -1;
    int64_t descriptorOffset = -1;
    D3D12_ROOT_PARAMETER_TYPE type = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
};

// Builds a layout from a deserialized desc (1.0 or 1.1; host tests feed hand-built descs).
RootLayout LayoutFromDesc(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC& desc);

// `kind`: CBV, SRV or UAV. Pixel-stage lookup: parameters visible only to other stages are skipped.
bool ResolveRegister(const RootLayout& layout, D3D12_DESCRIPTOR_RANGE_TYPE kind, uint32_t reg, uint32_t space, RootLocation* out);

// The game's root signatures (CreateRootSignature hook).
void NoteRootSignature(ID3D12RootSignature* rs, const void* blob, size_t size);
bool GetRootLayout(const ID3D12RootSignature* rs, RootLayout* out);
uint64_t RootSignaturesNoted();

// One line per parameter, for the log ("p0 CBV b4 s0 | p3 table SRV t0-t31 s0 ...").
std::string DescribeLayout(const RootLayout& layout);

} // namespace rtsky::track
