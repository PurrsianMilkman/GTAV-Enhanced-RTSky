// RTSky - precompiled shader bytecode (DXIL, generated at build time by DXC, see CMakeLists.txt)
#pragma once

#include <cstddef>

namespace rtsky::render {

enum class ShaderId
{
    Transmittance,
    MultiScatter,
    SkyView,
    SkyProject,
    Prepare,
    TraceInline,
    TraceLibrary, // DXIL library (raygen / miss / closest hit / any hit)
    Temporal,
    ATrous,
    Composite,
    Probe,
    DebugBlit,
    Count
};

struct ShaderBlob
{
    const void* data = nullptr;
    size_t size = 0;
};

ShaderBlob GetShader(ShaderId id);

} // namespace rtsky::render
