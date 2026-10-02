// RTSky - the game's passes named by their pixel shader's entry point
#include "PassNames.h"

#include <cstring>

namespace rtsky::track {
namespace {

struct NameEntry
{
    PassId id;
    const char* name;
};

const NameEntry kNames[] = {
    { PassId::DirectionalStandard, "PS_directional_standard" },
    { PassId::DirectionalScatter, "PS_directional_scatter" },
    { PassId::DirectionalJustDir, "PS_directional_just_dir_with_shadow" },
    { PassId::PuddleRtReflection, "PS_puddleMaskAndPassCombinedRTReflection" },
    { PassId::RtgiAccumulate, "PS_ApplyTemporalAccumulationLitColor_RTIndirectDiffuse" },
    { PassId::RtgiDenoiseY, "PS_DenoiseBilaterialSeparableYPass_RTIndirectDiffuse" },
    { PassId::RtgiDenoiseSmallY, "PS_DenoiseBilaterialSeparableSmallYPass_RTIndirectDiffuse" },
    { PassId::AoUpsample, "PS_Upsample" },
    { PassId::SkyCube, "ps_sky_water_reflection_all" },
    { PassId::SkyCubePlane, "ps_sky_plane_cubemap" },
    { PassId::LensDistortion, "PS_LensDistortion" },
    { PassId::CompositeExtraFx, "PS_CompositeExtraFX" },
    { PassId::GBufferTextured, "PS_DeferredTextured" },
};

constexpr char kWrapped[] = "_Wrapped";
constexpr size_t kWrappedLen = sizeof(kWrapped) - 1;

bool IsIdentChar(uint8_t c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

bool IsHex(uint8_t c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

uint32_t ReadU32(const uint8_t* p)
{
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

// First "<identifier>_Wrapped" in [p, p + n), as the identifier including the suffix.
std::string FindWrappedName(const uint8_t* p, size_t n)
{
    for (size_t i = 0; i + kWrappedLen <= n; ++i)
    {
        if (std::memcmp(p + i, kWrapped, kWrappedLen) != 0)
            continue;
        // The name ends right after "_Wrapped" (string table: NUL-terminated).
        if (i + kWrappedLen < n && IsIdentChar(p[i + kWrappedLen]))
            continue;
        size_t begin = i;
        while (begin > 0 && IsIdentChar(p[begin - 1]))
            --begin;
        if (begin < i)
            return std::string(reinterpret_cast<const char*>(p + begin), i + kWrappedLen - begin);
    }
    return {};
}

// "<name>_<8 hex>_Wrapped" -> "<name>"; anything else is returned unchanged.
std::string StripSuffix(const std::string& full)
{
    const size_t tail = 1 + 8 + kWrappedLen; // "_" + 8 hex + "_Wrapped"
    if (full.size() <= tail || full.compare(full.size() - kWrappedLen, kWrappedLen, kWrapped) != 0)
        return full;
    const size_t hexStart = full.size() - kWrappedLen - 8;
    if (full[hexStart - 1] != '_')
        return full;
    for (size_t i = hexStart; i < hexStart + 8; ++i)
    {
        if (!IsHex(static_cast<uint8_t>(full[i])))
            return full;
    }
    return full.substr(0, hexStart - 1);
}

} // namespace

std::string FindEntryName(const void* bytecode, size_t size)
{
    const uint8_t* p = static_cast<const uint8_t*>(bytecode);
    if (p == nullptr || size < 32)
        return {};
    // DXBC container: "DXBC", 16-byte digest, version, total size, part count, part offsets; each part
    // starts with its fourcc and size. Search the PSV0 part first (where the entry name lives).
    if (std::memcmp(p, "DXBC", 4) == 0)
    {
        const uint32_t parts = ReadU32(p + 28);
        for (uint32_t i = 0; i < parts && 32 + 4 * (i + 1) <= size; ++i)
        {
            const uint32_t off = ReadU32(p + 32 + 4 * i);
            if (off + 8 > size || std::memcmp(p + off, "PSV0", 4) != 0)
                continue;
            const uint32_t partSize = ReadU32(p + off + 4);
            if (off + 8 + static_cast<size_t>(partSize) > size)
                break;
            std::string name = FindWrappedName(p + off + 8, partSize);
            if (!name.empty())
                return StripSuffix(name);
            break;
        }
    }
    // Other layouts: the whole blob.
    std::string name = FindWrappedName(p, size);
    return name.empty() ? name : StripSuffix(name);
}

PassId PassIdFromEntryName(const std::string& entryName)
{
    for (const NameEntry& e : kNames)
    {
        if (entryName == e.name)
            return e.id;
    }
    return PassId::Unknown;
}

const char* PassName(PassId id)
{
    for (const NameEntry& e : kNames)
    {
        if (e.id == id)
            return e.name;
    }
    return "?";
}

} // namespace rtsky::track
