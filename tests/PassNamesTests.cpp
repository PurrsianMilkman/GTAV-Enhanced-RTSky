// RTSky - host tests of the pass-name scanner (src/Track/PassNames.cpp) on synthetic DXBC containers.
#include "Track/PassNames.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using rtsky::track::FindEntryName;
using rtsky::track::PassId;
using rtsky::track::PassIdFromEntryName;

namespace {

int g_failures = 0;

void Check(bool ok, const char* what)
{
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++g_failures;
}

void PutU32(std::vector<uint8_t>& b, size_t at, uint32_t v)
{
    std::memcpy(b.data() + at, &v, 4);
}

// A DXBC container with the given parts (fourcc, payload), laid out like dxc's output.
std::vector<uint8_t> Container(const std::vector<std::pair<std::string, std::string>>& parts)
{
    const size_t header = 32 + 4 * parts.size();
    std::vector<uint8_t> b(header, 0);
    std::memcpy(b.data(), "DXBC", 4);
    PutU32(b, 24, 1);
    PutU32(b, 28, static_cast<uint32_t>(parts.size()));
    for (size_t i = 0; i < parts.size(); ++i)
    {
        PutU32(b, 32 + 4 * i, static_cast<uint32_t>(b.size()));
        const size_t at = b.size();
        b.resize(at + 8 + parts[i].second.size());
        std::memcpy(b.data() + at, parts[i].first.data(), 4);
        PutU32(b, at + 4, static_cast<uint32_t>(parts[i].second.size()));
        std::memcpy(b.data() + at + 8, parts[i].second.data(), parts[i].second.size());
    }
    PutU32(b, 20, static_cast<uint32_t>(b.size()));
    return b;
}

std::string StringTable(const char* name)
{
    // PSV0 runtime info and resource table in front, then the NUL-terminated string table.
    std::string s(52, '\x01');
    s += '\0';
    s += name;
    s += '\0';
    return s;
}

std::string Name(const std::vector<uint8_t>& b)
{
    return FindEntryName(b.data(), b.size());
}

} // namespace

int main()
{
    std::printf("pass names in DXIL containers\n");
    {
        const auto b = Container({ { "SFI0", std::string(8, '\0') }, { "PSV0", StringTable("PS_directional_standard_d9a02477_Wrapped") },
                                   { "DXIL", "bitcode" } });
        Check(Name(b) == "PS_directional_standard", "entry name from the PSV0 string table, suffix stripped");
        Check(PassIdFromEntryName(Name(b)) == PassId::DirectionalStandard, "maps to DirectionalStandard");
    }
    {
        const auto b = Container({ { "PSV0", StringTable("PS_directional_standard_extra_0123abcd_Wrapped") } });
        Check(Name(b) == "PS_directional_standard_extra", "a longer name keeps its own identity");
        Check(PassIdFromEntryName(Name(b)) == PassId::Unknown, "a longer name never matches a shorter table entry");
    }
    {
        const auto b = Container({ { "PSV0", StringTable("PS_UpsampleBilateral_12345678_Wrapped") } });
        Check(PassIdFromEntryName(Name(b)) == PassId::Unknown, "PS_UpsampleBilateral is not PS_Upsample");
    }
    {
        const auto b = Container({ { "PSV0", StringTable("PS_Upsample_1a2b3c4d_Wrapped") } });
        Check(PassIdFromEntryName(Name(b)) == PassId::AoUpsample, "PS_Upsample maps to AoUpsample");
    }
    {
        const auto b = Container({ { "PSV0", StringTable("main") } });
        Check(Name(b).empty(), "no _Wrapped name: empty");
    }
    {
        // Not in PSV0 (other layout): found by the whole-blob fallback.
        const auto b = Container({ { "PSV0", StringTable("main") }, { "RDAT", std::string("xx\0ps_sky_water_reflection_all_0badf00d_Wrapped\0", 48) } });
        Check(Name(b) == "ps_sky_water_reflection_all", "fallback search outside PSV0");
        Check(PassIdFromEntryName(Name(b)) == PassId::SkyCube, "maps to SkyCube");
    }
    {
        const auto b = Container({ { "PSV0", StringTable("PS_Custom_Wrapped") } });
        Check(Name(b) == "PS_Custom_Wrapped", "no hex suffix: the full name is kept");
    }
    {
        std::vector<uint8_t> tiny = { 'D', 'X', 'B', 'C' };
        Check(FindEntryName(tiny.data(), tiny.size()).empty(), "truncated container: empty, no overrun");
        std::vector<uint8_t> bad = Container({ { "PSV0", StringTable("PS_directional_standard_d9a02477_Wrapped") } });
        PutU32(bad, 28, 1000); // part count past the end
        Check(PassIdFromEntryName(Name(bad)) == PassId::DirectionalStandard, "bogus part count: no overrun, fallback still finds it");
    }

    std::printf(g_failures == 0 ? "\nall pass-name tests passed\n" : "\n%d pass-name test(s) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
