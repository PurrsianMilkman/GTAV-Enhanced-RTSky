// RTSky - the game's passes named by their pixel shader's entry point
//
// GTA V Enhanced keeps the entry names of its pixel shaders in the DXIL container (the PSV0 part's
// string table), e.g. "PS_directional_standard_d9a02477_Wrapped". RTSky identifies the passes it
// injects after by these names instead of render-target formats and per-list ordinals. Only names are
// matched; no shader code is used or stored.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace rtsky::track {

enum class PassId : uint8_t
{
    Unknown = 0,
    DirectionalStandard, // the deferred sun + ambient lighting pass (the Composite goes after its binding)
    DirectionalScatter,
    DirectionalJustDir,
    PuddleRtReflection,
    RtgiAccumulate,
    RtgiDenoiseY,
    RtgiDenoiseSmallY,
    AoUpsample,
    SkyCube,
    SkyCubePlane,
    LensDistortion,
    CompositeExtraFx,
    GBufferTextured,
    Count
};

// Entry name of the pixel shader in `bytecode` (a DXBC/DXIL container), without the
// "_<8 hex>_Wrapped" suffix when it has one; empty when none is found.
std::string FindEntryName(const void* bytecode, size_t size);

// The pass a pixel shader's entry name stands for (Unknown for any name not in the table).
PassId PassIdFromEntryName(const std::string& entryName);

const char* PassName(PassId id);

// Bit for BindingRecord::passMask.
inline uint32_t PassBit(PassId id) { return id == PassId::Unknown ? 0u : (1u << static_cast<uint32_t>(id)); }

} // namespace rtsky::track
