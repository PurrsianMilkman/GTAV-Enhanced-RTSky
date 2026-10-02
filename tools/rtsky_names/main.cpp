// rtsky_names - runs RTSky's pass-name scanner (src/Track/PassNames.cpp) over a directory of
// captured pixel shaders (RTSky_shaders\ps_<hash>.dxil) and prints one line per file, then the counts.
// Development tool only; not part of the release.
#include "Track/PassNames.h"

#include <windows.h>

#include <cstdio>
#include <map>
#include <string>
#include <vector>

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2)
    {
        std::fwprintf(stderr, L"usage: rtsky_names <directory of ps_*.dxil>\n");
        return 2;
    }
    const std::wstring dir = argv[1];
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((dir + L"\\ps_*.dxil").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE)
    {
        std::fwprintf(stderr, L"no ps_*.dxil in %ls\n", dir.c_str());
        return 1;
    }
    size_t files = 0, named = 0;
    std::map<rtsky::track::PassId, size_t> perPass;
    do
    {
        FILE* f = _wfopen((dir + L"\\" + fd.cFileName).c_str(), L"rb");
        if (f == nullptr)
            continue;
        std::vector<unsigned char> blob;
        unsigned char buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
            blob.insert(blob.end(), buf, buf + n);
        std::fclose(f);
        ++files;
        const std::string name = rtsky::track::FindEntryName(blob.data(), blob.size());
        const rtsky::track::PassId id = rtsky::track::PassIdFromEntryName(name);
        if (!name.empty())
            ++named;
        ++perPass[id];
        std::wprintf(L"%ls %hs%hs%hs\n", fd.cFileName, name.empty() ? "?" : name.c_str(),
                     id == rtsky::track::PassId::Unknown ? "" : "  -> ", id == rtsky::track::PassId::Unknown ? "" : rtsky::track::PassName(id));
    } while (FindNextFileW(find, &fd));
    FindClose(find);

    std::printf("\nfiles %zu, named %zu, unnamed %zu\n", files, named, files - named);
    for (int i = 1; i < static_cast<int>(rtsky::track::PassId::Count); ++i)
    {
        const auto id = static_cast<rtsky::track::PassId>(i);
        std::printf("  %-60s %zu\n", rtsky::track::PassName(id), perPass[id]);
    }
    return named == files ? 0 : 1;
}
