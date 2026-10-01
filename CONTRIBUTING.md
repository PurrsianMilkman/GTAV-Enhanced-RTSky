# Contributing to RTSky

Thanks for helping. RTSky was written without access to the game, so **in-game testing is the most
valuable contribution right now**, followed by fixes that come out of it.

> Story mode only. Never use RTSky, or any mod, in GTA Online.

## Ways to help

* **Test it in game** and open an issue with the *In-game test report* template. Attach `RTSky.log`
  and `RTSky_frame.log` (Num .), plus a screenshot of the on-screen status lines. Reports from different GPUs (NVIDIA / AMD / Intel), drivers and
  game settings are all useful, including "it does nothing" reports: the log says why.
* **Tune the detection and look** for the real game: `CompositeCandidate`, the ordinals, depth mode,
  default strengths. Frame dumps from real frames are what this needs.
* **Code**: bugs, robustness, performance, new features. See [Open areas](#open-areas) below.

## Project layout

| Path | What |
|---|---|
| `src/Hooks` | D3D12CreateDevice detour, vtable patching (`VTableHook`), hook bodies, slot indices |
| `src/Track` | Per-command-list state (`CommandListTracker`), descriptors, TLAS capture, frame analysis |
| `src/Render` | The renderer: resources, the Prepare / Composite injections, barriers, DXR pipeline, GPU lifetimes, calibration |
| `src/Game` | ScriptHookV bridge, camera / clock / weather samples, sun model, weather table |
| `src/Common` | Config (INI), logging, math, MinGW / MSVC compatibility helpers |
| `shaders` | HLSL: atmosphere LUTs, SH projection, prepare, trace (DXR library + inline), denoiser, composite, probe. `RTSkyShared.h` is shared with C++ |
| `tests` | Host tests (math, frame analyzer) that build without the Windows SDK |
| `docs` | `ARCHITECTURE.md` (design), `CALIBRATION.md` (troubleshooting and tuning) |

Read [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) before changing anything in `src/Hooks`, `src/Track`
or `src/Render`: injecting into another program's command lists has rules that are easy to break.

## Building

* **Windows (MSVC 2022)**: `cmake -S . -B build -A x64` and `cmake --build build --config Release`.
  DXC comes from the Windows SDK (10.0.22621+); DirectX-Headers are fetched automatically.
* **Linux (MinGW-w64 cross)**: see the README. Pass `-DRTSKY_DXC=<path>` to a DXC build.

Shaders are compiled with `-WX`, and the C++ with `/W4` (MSVC) or `-Wall -Wextra` (MinGW). Keep both
warning-free.

## Tests

```sh
cmake -S tests -B build-tests -DRTSKY_DIRECTX_HEADERS_DIR=/path/to/DirectX-Headers
cmake --build build-tests && ctest --test-dir build-tests --output-on-failure
```

* `RTSkyMathTests`: camera basis, sun / moon orbit, weather.
* `RTSkyAnalyzerTests`: runs the real `FrameAnalyzer` / `CommandListTracker` sources against
  simulated frames, on any platform, through DirectX-Headers' WSL stubs and `tests/shim`. **Any
  change to the frame analysis needs a scenario here.** A frame dump from a real game makes a good
  test case.

CI (`.github/workflows/build.yml`) runs both test sets on Linux and builds the `.asi` with MSVC on
every push and pull request.

## Rules that keep the game alive

These are the invariants the code relies on. A change that breaks one usually means a crash, a
device removal or corrupted frames, and only on some machines.

1. **Everything RTSky records or creates runs under `hooks::HookBypass`**, so its own calls are never
   tracked as game state.
2. **Every injection restores what it changed**: game resources go back to their exact state
   (`TransitionGame` / `RestoreGame`, with the barrier API the game uses on that resource), and
   heaps, root signatures, root arguments and the pipeline go back through `track::RestoreState`.
   Never return early between `BeginPasses` and the restore.
3. **No CPU thread ever waits on the GPU.** Objects referenced by recorded commands are attached
   to the list (`Lifetime().Attach`) and released when the submission's fence passes.
4. **Recording order is not GPU order.** Anything that depends on another list (TLAS clones, the
   Prepare / Composite pairing) is resolved at `ExecuteCommandLists` or checked there.
5. **Never trace the game's own TLAS memory**, only RTSky's clone of it, and never make one queue
   wait for another (that can deadlock with the game's own cross-queue waits).
6. **`shaders/RTSkyShared.h` is the single definition** of the constant-buffer layout, registers and
   flags shared by HLSL and C++. Change both sides together; the 1024-byte `FrameConstants` limit is
   checked at compile time.

## Code style

Match the surrounding code: 4-space indentation, braces on their own line, `m_` members, `k`
constants. Comments explain *why* (D3D12 rules, game behaviour, failure modes) rather than *what*.
Prefer a skipped frame with a logged reason (`m_lastSkip`, `RTSKY_LOG_ONCE`) over a guess that could
corrupt the game's state.

## Pull requests

* One topic per PR, with a description of the change and why it is needed.
* CI must be green: MSVC build, shaders, host tests.
* Update `docs/` and `config/RTSky.ini` when behaviour or options change.
* Say whether and how you tested in game (GPU, driver, settings), or that you did not.

## Releases

Maintainers publish a release from the Actions tab: **release** workflow, **Run workflow** on `main`
with a tag such as `v0.2.0` or `v0.2.0-beta`. Pushing such a tag does the same. The workflow builds
with MSVC, runs the tests, and attaches `RTSky-<tag>.zip` and its SHA-256 to a GitHub release. Tags
with a hyphen become pre-releases. Bump `project(RTSky VERSION ...)` in `CMakeLists.txt` with each
release.

## Open areas

* Real-game validation of the pass detection, the camera / TLAS calibration and the barrier handling
  on NVIDIA, AMD and Intel.
* A Prepare that works when the G-buffer is split across several command lists at the same ordinal.
  Today RTSky refuses to arm in that case.
* Sampling the game's alpha textures for foliage instead of procedural coverage.
* Performance: half-resolution tracing, fewer denoiser iterations on low-end GPUs.

## License

RTSky is licensed under the GNU General Public License v3.0 ([LICENSE](LICENSE)). By contributing
you agree that your contributions are licensed under the same terms.
