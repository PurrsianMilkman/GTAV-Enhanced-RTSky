cmake_minimum_required(VERSION 3.21)
project(RTSky VERSION 0.2.0 LANGUAGES C CXX)

# -------------------------------------------------------------------------------------------------
# Options
# -------------------------------------------------------------------------------------------------
set(RTSKY_DXC "" CACHE FILEPATH "Path to dxc(.exe). Defaults to the Windows SDK / PATH.")
set(RTSKY_DIRECTX_HEADERS_DIR "" CACHE PATH "Root of a DirectX-Headers checkout (leave empty to fetch v1.619.5).")
set(RTSKY_VERSION_STRING "" CACHE STRING "Version written to RTSky.log (release tag or commit); empty = <project version>-dev.")
# Not a cached default: a cached "<version>-dev" froze at the first configure and outlived version bumps.
if(NOT RTSKY_VERSION_STRING)
    set(RTSKY_VERSION_STRING "${PROJECT_VERSION}-dev")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "RTSky is a 64-bit mod: configure an x64 build.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# -------------------------------------------------------------------------------------------------
# DirectX-Headers (newest d3d12.h: DXR 1.1/1.2, enhanced barriers, d3dx12 helpers)
# -------------------------------------------------------------------------------------------------
if(RTSKY_DIRECTX_HEADERS_DIR)
    set(DXH_ROOT "${RTSKY_DIRECTX_HEADERS_DIR}")
else()
    include(FetchContent)
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)
        # Headers only: point SOURCE_SUBDIR at a directory without CMakeLists.txt so that nothing of
        # DirectX-Headers' own build (tests, targets) is added.
        FetchContent_Declare(directx_headers
            GIT_REPOSITORY https://github.com/microsoft/DirectX-Headers.git
            GIT_TAG v1.619.5
            GIT_SHALLOW TRUE
            SOURCE_SUBDIR include)
        FetchContent_MakeAvailable(directx_headers)
    else()
        FetchContent_Declare(directx_headers
            GIT_REPOSITORY https://github.com/microsoft/DirectX-Headers.git
            GIT_TAG v1.619.5
            GIT_SHALLOW TRUE)
        FetchContent_GetProperties(directx_headers)
        if(NOT directx_headers_POPULATED)
            FetchContent_Populate(directx_headers)
        endif()
    endif()
    set(DXH_ROOT "${directx_headers_SOURCE_DIR}")
endif()

# -------------------------------------------------------------------------------------------------
# Shaders -> DXIL headers (dxc -Fh)
# -------------------------------------------------------------------------------------------------
if(NOT RTSKY_DXC)
    # Newest Windows SDK first, then PATH.
    file(GLOB _sdk_dxc "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/*/x64/dxc.exe")
    if(_sdk_dxc)
        list(SORT _sdk_dxc COMPARE NATURAL ORDER DESCENDING)
        list(GET _sdk_dxc 0 RTSKY_DXC)
    else()
        find_program(_path_dxc NAMES dxc dxc.exe)
        if(_path_dxc)
            set(RTSKY_DXC "${_path_dxc}")
        endif()
    endif()
endif()
if(NOT RTSKY_DXC)
    message(FATAL_ERROR "dxc not found. Install the Windows SDK (10.0.22621+) or pass -DRTSKY_DXC=<path to dxc>.")
endif()
message(STATUS "RTSky: using DXC ${RTSKY_DXC}")

set(SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GEN_DIR}")
file(GLOB SHADER_DEPENDENCIES "${SHADER_DIR}/*.hlsl" "${SHADER_DIR}/*.hlsli" "${SHADER_DIR}/*.h")

set(RTSKY_SHADER_HEADERS "")
# name|file|entry (or -)|profile
set(RTSKY_SHADERS
    "Transmittance|AtmosphereCS.hlsl|TransmittanceCS|cs_6_0"
    "MultiScatter|AtmosphereCS.hlsl|MultiScatterCS|cs_6_0"
    "SkyView|AtmosphereCS.hlsl|SkyViewCS|cs_6_0"
    "SkyProject|SkyProjectCS.hlsl|SkyProjectCS|cs_6_0"
    "Prepare|PrepareCS.hlsl|PrepareCS|cs_6_0"
    "TraceInline|RTSkyInlineCS.hlsl|TraceInlineCS|cs_6_5"
    "TraceLibrary|RTSkyLib.hlsl|-|lib_6_3"
    "Temporal|TemporalCS.hlsl|TemporalCS|cs_6_0"
    "ATrous|ATrousCS.hlsl|ATrousCS|cs_6_0"
    "Composite|CompositeCS.hlsl|CompositeCS|cs_6_0"
    "Probe|ProbeCS.hlsl|ProbeCS|cs_6_5"
    "DebugBlit|DebugBlitCS.hlsl|DebugBlitCS|cs_6_0")

foreach(_spec IN LISTS RTSKY_SHADERS)
    string(REPLACE "|" ";" _parts "${_spec}")
    list(GET _parts 0 _name)
    list(GET _parts 1 _file)
    list(GET _parts 2 _entry)
    list(GET _parts 3 _profile)
    set(_out "${GEN_DIR}/RTSkyShader_${_name}.h")
    set(_entry_args "")
    if(NOT _entry STREQUAL "-")
        set(_entry_args -E ${_entry})
    endif()
    add_custom_command(
        OUTPUT "${_out}"
        COMMAND "${RTSKY_DXC}" -nologo -T ${_profile} ${_entry_args} -HV 2021 -WX -O3 -Qstrip_debug -Qstrip_reflect
                -I "${SHADER_DIR}" -Fh "${_out}" -Vn g_${_name} "${SHADER_DIR}/${_file}"
        DEPENDS ${SHADER_DEPENDENCIES}
        COMMENT "DXC ${_file} (${_profile} ${_entry})"
        VERBATIM)
    list(APPEND RTSKY_SHADER_HEADERS "${_out}")
endforeach()
add_custom_target(RTSkyShaders DEPENDS ${RTSKY_SHADER_HEADERS})

# -------------------------------------------------------------------------------------------------
# MinHook (BSD-2-Clause, vendored)
# -------------------------------------------------------------------------------------------------
add_library(minhook STATIC
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
    third_party/minhook/src/hde/hde64.c)
target_include_directories(minhook PUBLIC third_party/minhook/include)

# -------------------------------------------------------------------------------------------------
# RTSky.asi
# -------------------------------------------------------------------------------------------------
add_library(RTSky SHARED
    src/Main.cpp
    src/Common/Config.cpp
    src/Common/Log.cpp
    src/Game/GameData.cpp
    src/Game/Overlay.cpp
    src/Game/ScriptHookV.cpp
    src/Game/SunModel.cpp
    src/Game/Weather.cpp
    src/Hooks/D3D12Hooks.cpp
    src/Hooks/VTableHook.cpp
    src/Hooks/VTableIndices.c
    src/Render/Barriers.cpp
    src/Render/Calibration.cpp
    src/Render/GpuLifetime.cpp
    src/Render/InputCensus.cpp
    src/Render/Renderer.cpp
    src/Render/RtPipeline.cpp
    src/Render/ShaderBlobs.cpp
    src/Track/CommandListTracker.cpp
    src/Track/DescriptorTracker.cpp
    src/Track/FrameAnalyzer.cpp
    src/Track/BufferTracker.cpp
    src/Track/PassNames.cpp
    src/Track/RootSignatureTracker.cpp
    src/Track/ShaderCapture.cpp
    src/Track/TlasTracker.cpp
    ${RTSKY_SHADER_HEADERS})
add_dependencies(RTSky RTSkyShaders)
set_target_properties(RTSky PROPERTIES OUTPUT_NAME "RTSky" PREFIX "" SUFFIX ".asi")

# DirectX-Headers' d3d12.h must win over the SDK / MinGW copy.
target_include_directories(RTSky BEFORE PRIVATE "${DXH_ROOT}/include/directx" "${DXH_ROOT}/include")
target_include_directories(RTSky PRIVATE src shaders "${GEN_DIR}")
target_link_libraries(RTSky PRIVATE minhook)
target_compile_definitions(RTSky PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE RTSKY_VERSION="${RTSKY_VERSION_STRING}")

if(MSVC)
    target_compile_options(RTSky PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /wd4100 /wd4324)
    target_compile_definitions(RTSky PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(RTSky PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
    # MinGW ships rpcndr.h version 475; DirectX-Headers only asks for >= 500 as a MIDL sanity check.
    target_compile_definitions(RTSky PRIVATE __REQUIRED_RPCNDR_H_VERSION__=475)
    target_link_options(RTSky PRIVATE -static -static-libgcc -static-libstdc++ -Wl,--enable-stdcall-fixup)
endif()

install(TARGETS RTSky RUNTIME DESTINATION . LIBRARY DESTINATION .)
install(FILES config/RTSky.ini DESTINATION .)

# -------------------------------------------------------------------------------------------------
# Host-side tests of the platform independent math (camera basis, sun orbit, weather)
# -------------------------------------------------------------------------------------------------
option(RTSKY_BUILD_TESTS "Build the math tests" OFF)
if(RTSKY_BUILD_TESTS)
    enable_testing()
    add_executable(RTSkyMathTests tests/MathTests.cpp src/Game/SunModel.cpp src/Game/Weather.cpp)
    target_include_directories(RTSkyMathTests PRIVATE src)
    add_test(NAME RTSkyMathTests COMMAND RTSkyMathTests)
    add_executable(RTSkyPassNamesTests tests/PassNamesTests.cpp src/Track/PassNames.cpp)
    target_include_directories(RTSkyPassNamesTests PRIVATE src)
    add_test(NAME RTSkyPassNamesTests COMMAND RTSkyPassNamesTests)
    # The frame-analyzer scenarios against the real tracker / analyzer sources (on Windows the SDK's own
    # headers stand in for the Linux shims of tests/CMakeLists.txt).
    add_executable(RTSkyAnalyzerTests tests/AnalyzerTests.cpp src/Track/FrameAnalyzer.cpp src/Track/CommandListTracker.cpp
                   src/Track/DescriptorTracker.cpp src/Track/PassNames.cpp)
    target_include_directories(RTSkyAnalyzerTests BEFORE PRIVATE "${DXH_ROOT}/include/directx" "${DXH_ROOT}/include")
    target_include_directories(RTSkyAnalyzerTests PRIVATE src)
    target_link_libraries(RTSkyAnalyzerTests PRIVATE dxguid)
    # The analyzer reads GetTickCount64; the scenarios advance a fake clock instead (tests/shim on Linux).
    target_compile_definitions(RTSkyAnalyzerTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
                               RTSKY_TEST_CLOCK)
    target_compile_options(RTSkyAnalyzerTests PRIVATE /W4 /wd4100)
    add_test(NAME RTSkyAnalyzerTests COMMAND RTSkyAnalyzerTests)
    add_executable(RTSkyRootSignatureTests tests/RootSignatureTests.cpp src/Track/RootSignatureTracker.cpp src/Common/Log.cpp)
    target_include_directories(RTSkyRootSignatureTests BEFORE PRIVATE "${DXH_ROOT}/include/directx" "${DXH_ROOT}/include")
    target_include_directories(RTSkyRootSignatureTests PRIVATE src)
    target_compile_definitions(RTSkyRootSignatureTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_compile_options(RTSkyRootSignatureTests PRIVATE /W4)
    add_test(NAME RTSkyRootSignatureTests COMMAND RTSkyRootSignatureTests)
endif()

# Development tool: the pass-name scanner over a directory of captured shaders (not released)
option(RTSKY_BUILD_TOOLS "Build the development tools" OFF)
if(RTSKY_BUILD_TOOLS)
    add_executable(rtsky_names tools/rtsky_names/main.cpp src/Track/PassNames.cpp)
    target_include_directories(rtsky_names PRIVATE src)
    target_compile_definitions(rtsky_names PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
endif()
