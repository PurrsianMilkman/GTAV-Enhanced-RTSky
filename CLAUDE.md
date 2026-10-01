# RTSky: notes for Claude Code sessions

Start with **[HANDOFF.md](HANDOFF.md)**: project goal, current state (open problem: no visible change
in game yet), how to build, test and release, known limitations, and next steps. Then read
`docs/ARCHITECTURE.md` before touching `src/Hooks`, `src/Track` or `src/Render`.

## Rules

- **Native D3D12 mod with world-space DXR against the game's own TLAS.** Never propose ReShade or
  screen-space tracing; the owner rejected that explicitly.
- **Keep the invariants in `CONTRIBUTING.md` ("Rules that keep the game alive"):**
  - `HookBypass` around everything RTSky records;
  - exact state restore;
  - no CPU waits on the GPU;
  - resolve GPU order at ExecuteCommandLists;
  - never trace the game's live TLAS;
  - `shaders/RTSkyShared.h` is the single C++/HLSL layout.
- **Every change to frame analysis** needs a scenario in `tests/AnalyzerTests.cpp`.
- **Keep builds warning-free:** MSVC `/W4`, MinGW `-Wall -Wextra`, DXC `-WX`.
- **Keep docs in sync:** update `docs/`, `config/RTSky.ini` and the README hotkey table when behaviour
  or options change.
- **Style:** match the surrounding code (4 spaces, braces on new lines, `m_` members, comments explain
  why).
- **Releases:**
  1. Bump `project(RTSky VERSION ...)`.
  2. Merge to `main`.
  3. Run the **release** workflow with a `vX.Y.Z[-suffix]` tag.

  The owner prefers that a green PR is merged and released without waiting for them.

## Quick commands

```sh
# host tests (any OS)
cmake -S tests -B build-tests -DRTSKY_DIRECTX_HEADERS_DIR=<DirectX-Headers> && cmake --build build-tests && ctest --test-dir build-tests
# Windows build
cmake -S . -B build -A x64 -DRTSKY_BUILD_TESTS=ON && cmake --build build --config Release
```
