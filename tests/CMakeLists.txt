# RTSky host tests - a standalone project that builds without the Windows SDK or DXC:
#   cmake -S tests -B build-tests -DRTSKY_DIRECTX_HEADERS_DIR=<DirectX-Headers checkout>
#   cmake --build build-tests && ctest --test-dir build-tests --output-on-failure
#   * RTSkyMathTests     - camera basis, sun / moon orbit, weather (any platform)
#   * RTSkyAnalyzerTests - frame analyzer scenarios against the real tracker / analyzer sources
#                          (non-Windows: DirectX-Headers' WSL stubs + tests/shim stand in for Win32)
cmake_minimum_required(VERSION 3.21)
project(RTSkyHostTests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()

set(RTSKY_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../src")
set(RTSKY_DIRECTX_HEADERS_DIR "" CACHE PATH "Root of a DirectX-Headers checkout (analyzer tests)")

add_executable(RTSkyMathTests MathTests.cpp "${RTSKY_SRC}/Game/SunModel.cpp" "${RTSKY_SRC}/Game/Weather.cpp")
target_include_directories(RTSkyMathTests PRIVATE "${RTSKY_SRC}")
add_test(NAME math COMMAND RTSkyMathTests)

if(RTSKY_DIRECTX_HEADERS_DIR AND NOT WIN32)
    find_package(Threads REQUIRED)
    add_executable(RTSkyAnalyzerTests AnalyzerTests.cpp
        "${RTSKY_SRC}/Track/FrameAnalyzer.cpp"
        "${RTSKY_SRC}/Track/CommandListTracker.cpp"
        "${RTSKY_SRC}/Track/DescriptorTracker.cpp")
    target_include_directories(RTSkyAnalyzerTests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/shim"
        "${RTSKY_DIRECTX_HEADERS_DIR}/include"
        "${RTSKY_DIRECTX_HEADERS_DIR}/include/directx"
        "${RTSKY_DIRECTX_HEADERS_DIR}/include/wsl/stubs"
        "${RTSKY_SRC}")
    target_compile_options(RTSkyAnalyzerTests PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/shim/prelude.h"
        -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
    target_link_libraries(RTSkyAnalyzerTests PRIVATE Threads::Threads)
    add_test(NAME analyzer COMMAND RTSkyAnalyzerTests)
endif()
