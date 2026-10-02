# RTSky handoff

This document hands the RTSky project from the cloud development session that built it to a new
session (for example Claude Code running locally on the Windows machine that has the game). Read it
first, then `docs/ARCHITECTURE.md`. It records the goal, where things stand, what was decided and why,
what is known to be uncertain, and what to do next.

*State as of 2026-10-01: latest release **v0.1.4-alpha** (v0.1.2 and later come from a local session on the
owner's machine). The game is at `C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto V
Enhanced`; its `RTSky.log` can be read directly.*

---

## 1. The goal

**What the owner asked for:** a custom ray-tracing shader for **sky lighting in GTA V Enhanced** (the 2025
DirectX 12 PC edition). The sky should be ray traced and cast proper shadows. The game ray traces GI,
reflections, shadows and AO, but its sky / ambient light is faked: a hemispherical gradient times
baked AO.

**Hard requirements from the owner, do not regress:**

- **Native, not ReShade.** An early direction toward ReShade was explicitly rejected: *"this is supposed
  to be a native shader for the game for world space ray tracing"*.
- **World-space rays** traced against **the game's own scene BVH (TLAS)**. Only ray *origins* come
  from the depth buffer. It is not screen-space.
- Story mode only (never GTA Online). License **GPL-3.0** (the owner's choice).
- Hotkeys on the **numpad**: Num 1 is the activation toggle, and the other numpad keys switch the
  remaining toggles (the owner's request, implemented in v0.1.1).
- **The sky only** (the owner's clarification, 2026-10-01): *"this shader is meant to ray trace only
  the sky and nothing else. the sun is already in the global illumination. the sky is not ... i want
  this mod to use the true colors of what the sky sees."* RTSky must never add or change sun light.
  Today the sky radiance comes from RTSky's own Hillaire atmosphere, not from the game's sky, which
  does not meet "the true colors"; sourcing it from the game's real sky (for example the sky lookup
  its RT reflections / GI use on a miss) is open work. Whether to keep the sun ray (it only weights
  the ratio, see section 5) is the owner's call, still to be asked.

**How the owner likes to work:** when a PR is green, they said *"Do it yourself"*: merge it and
publish the release rather than waiting for them. They test in game and report back. Usage limits
sometimes cut the cloud session off mid-task; "continue" or "try again" means resume where it
stopped.

---

## 2. Where things stand

| Item | State |
|---|---|
| Code | Complete: 11 HLSL shaders and the native C++ host, about 12K lines of code and docs |
| Build | MSVC CI (Windows) and Linux host tests are green on every push. A local MinGW cross-build is also clean |
| Tests | Host tests pass: `RTSkyMathTests` (17 checks) and `RTSkyAnalyzerTests` (23 checks in 11 scenarios) |
| Reviews | 4 adversarial review rounds (D3D12 sync, hooks/threads, shaders/math, integration, robustness, plus re-reviews of the fixes). Every confirmed finding is fixed except the deliberate exceptions in section 7 |
| Releases | [v0.1.0-alpha](https://github.com/PurrsianMilkman/GTAV-Enhanced-RTSky/releases/tag/v0.1.0-alpha) and [v0.1.1-alpha](https://github.com/PurrsianMilkman/GTAV-Enhanced-RTSky/releases/tag/v0.1.1-alpha), both pre-releases with `RTSky-<tag>.zip` and its SHA-256 |
| **In-game result** | **The owner ran v0.1.0-alpha: "the shader does not seem to make any visual changes."** No log was shared, so **the cause is unknown** |
| Response | v0.1.1-alpha adds an on-screen status display whose first line names the first stage that stops RTSky. It also adds split-screen compare (Num 3), force relight (Num 4) and numpad hotkeys |
| **v0.1.1 in game** | `RTSky.log` from the owner's run: hooks, ScriptHookV, hotkeys and TLAS capture all work. Two blockers. **(1)** The analyzer never armed: `G-buffer 5 RT [BGRA8_UNORM x4, RG16F] + D32S8 1707x960: the G-buffer or HDR pass is not unique per frame` for the whole session, so 0 Prepares / 0 Composites and nothing could change on screen. No frame dump was taken. **(2)** TLAS clone buffers grew 25% per build to 4.7 GB each, with 125 failed allocations (E_OUTOFMEMORY): slots were busy until the game reset its lists, so every build took the all-busy path, which sized from the old buffer |
| v0.1.2 | Fixes (2): busy tokens are released when the GPU finishes a list's first execution (memory kept separately), the ring has 8 slots, and all-busy buffers are sized from the build. The same fix applies to the renderer's injection slots. For (1): the status and the dump name the ambiguous pass and its counts, and RTSky writes `RTSky_frame.log` on its own after 3 s of ambiguity |

| **v0.1.2 in game** | The clone fix holds: 15 allocations in the first second, 640 KB each, then none. Still no visible change: `not unique per frame - G-buffer mrt#0 x12..x18 per frame (x.. after 0 hdr in its list)`. The auto dump showed why: **the G-buffer is recorded in ~15 parallel command lists**, each binding `5 RT [BGRA8 x4, RG16F] + D32S8` at its first binding, all on the direct queue before the lighting lists. Also in the dump: a 1024² six-face cubemap pass (depth clear 1.0), lit into a 2048x1024 R11G11B10F map with mips - very likely GTA's environment / reflection map, which holds the game's real sky (candidate source for the sky colours) |
| v0.1.3 | Fixes the arming: when the G-buffer rule repeats only inside the G-buffer phase, Prepare goes after **every** such binding (`InjectionRules::gbufferEvery`), and all Prepares recorded since the last Composite form one group (one PendingFrame, parity, camera and constants slot), so the last one on the GPU leaves the complete depth. The dump prints `rt0=` / `ds=` resource pointers to tell which passes write the scene lighting buffer |

| **v0.1.3 in game** | **RTSky runs**: armed in every-list mode (~13.4 Prepares per Composite), DXR pipeline created, calibration passed at once (latency 0, TLAS **camera-relative**, score 0.99-1.00), "ACTIVE - relighting". The owner reports the debug views are **"mixed into the scene"** (not a flat image). The Composite sits on the first full-res float pass after the G-buffer, `2 RT [RGBA16F,R16F] 1707x960`, a 1-draw pass followed by half-res R8 ping-pong passes and more 1-draw RGBA16F passes - most likely an intermediate lighting term (GI / ambient / AO, blurred and combined later), not the scene colour. What it holds decides whether it is the right target, so it is being identified by its shader rather than guessed |
| v0.1.4 | Diagnostics: pipeline-creation hooks (device `CreateGraphicsPipelineState` / `CreatePipelineState`, `ID3D12PipelineLibrary1` loads) hash each pixel shader; the dump lists `ps=<hash>` per binding, and `[Detection] CaptureShaders=1` writes each shader once to `RTSky_shaders\ps_<hash>.dxil` for `dxc -dumpbin`. The status line counts `pipelines noted`. Also fixed: a cached CMake default froze the local version string at `0.1.1-dev` |

### The immediate next step

Run v0.1.4 with `CaptureShaders=1` (set before starting the game), press Num . in gameplay, then:

- disassemble the pixel shaders of the COMPOSITE pass and of the other full-res float passes after
  the G-buffer (`dxc -dumpbin RTSky_shaders\ps_<hash>.dxil`): which G-buffer channels, textures and
  constants each reads and what it writes. Find the pass where the sky / ambient term is applied
  to the scene colour, and the buffer that carries the scene colour onwards (`rt0=`);
- point the Composite there (`CompositeCandidate` / `CompositeOrdinal`, or a new discriminator if
  ordinals collide), and adapt `E_keep` if the target holds only an ambient term;
- `pipelines noted` near 0 means the game creates pipelines some other way: find it before going on.

Then follow section 6.

---

## 3. Repository, branches, PRs, releases

**Remote:** `github.com/PurrsianMilkman/GTAV-Enhanced-RTSky`. Default branch `main`.

**Merged PRs:**

| PR | What it did |
|---|---|
| #1 | Everything up to v0.1.0: full mod, docs, CI, license, templates |
| #2 | Release workflow can be run from the Actions tab and creates the tag itself |
| #3 | Numpad hotkeys, on-screen status, split compare, force relight (v0.1.1) |

**Cloud work branch:** `claude/gta-v-ray-traced-sky-us09if` (the cloud session could only push this
branch). A local session can use any branch.

**Publishing a release:**

1. Bump `project(RTSky VERSION x.y.z)` in `CMakeLists.txt`.
2. Merge to `main`.
3. Do either of these:
   - **Actions → release → Run workflow** on `main`, with tag `vX.Y.Z[-suffix]`;
   - or `git tag vX.Y.Z && git push origin vX.Y.Z`.

The workflow builds with MSVC, runs the math tests, and publishes `RTSky-<tag>.zip`, which contains
`.asi`, `.ini`, `README`, `LICENSE` and `docs/`. A hyphenated tag becomes a pre-release, and re-running
replaces the assets. The cloud session had to use the Actions route because its git proxy refused tag
pushes.

**Version string:** `RTSKY_VERSION_STRING` (CMake cache) is compiled in and printed on the first line
of `RTSky.log` and in the status overlay. Release builds use the tag, CI builds use `dev-<sha>`, and
local builds default to `<project version>-dev`.

---

## 4. Building, running, testing locally

### Windows (MSVC 2022), the normal local setup

```bat
cmake -S . -B build -A x64 -DRTSKY_BUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build -C Release
```

- DXC comes from the Windows SDK (10.0.22621+); CMake finds the newest one. DirectX-Headers v1.619.5 is
  fetched by CMake.
- The output is `build/Release/RTSky.asi`. Copy it with `config/RTSky.ini` into the game folder next to
  ScriptHookV for Enhanced and its ASI loader.
- Logs land next to the game executable: `RTSky.log`, and `RTSky_frame.log` after Num . .

### Linux cross-build (how the cloud session worked)

```sh
cmake -S . -B build-mingw -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake \
      -DCMAKE_BUILD_TYPE=Release -DRTSKY_DXC=/path/to/dxc -DRTSKY_DIRECTX_HEADERS_DIR=/path/to/DirectX-Headers
cmake --build build-mingw
```

- The cloud session built DXC from source, because the GitHub DXC release download was blocked.
- Only `x86_64-w64-mingw32-g++-posix` works (the posix threads variant).
- MinGW quirks are already handled in the code:
  - `__REQUIRED_RPCNDR_H_VERSION__=475`;
  - `<dxguids/dxguids.h>` for IIDs;
  - struct-returning COM methods go through `src/Common/D3D12Compat.h`.

### Host tests (any OS, no Windows SDK)

```sh
cmake -S tests -B build-tests -DRTSKY_DIRECTX_HEADERS_DIR=/path/to/DirectX-Headers
cmake --build build-tests && ctest --test-dir build-tests --output-on-failure
```

`RTSkyAnalyzerTests` compiles the real `FrameAnalyzer` and `CommandListTracker` against
DirectX-Headers' WSL stubs plus `tests/shim/` (Win32 shims and a forced-include prelude for IIDs).
**Every change to frame analysis needs a scenario there.** A real frame dump makes a good test case.

### In game

- Story mode, ray tracing enabled (any RT option), with ScriptHookV for Enhanced plus its ASI loader.
  RTSky resolves ScriptHookV's decorated exports with `GetProcAddress`.
- **Hotkeys need NumLock on.** All of them can be rebound in `[Hotkeys]` by name or VK code.

| Key | Action |
|---|---|
| Num 1 | on / off |
| Num 2 | status overlay |
| Num 3 | split compare (left original, right relit) |
| Num 4 | force relight while calibrating |
| Num 5 | next debug view |
| Num 6 | sun shadows |
| Num 7 | foliage mode |
| Num 8 | near-field |
| Num 9 | trace path |
| Num 0 | reload ini |
| Num . | frame dump |
| Num + / Num - | strength |
| Num * | denoiser |
| Num / | reset history and calibration |

- **Debug views:**
  - 1 sky visibility R
  - 2 sun visibility
  - 3 normals
  - 4 depth
  - 5 sky irradiance
  - **6 TLAS alignment** (green means aligned, red means a mismatch, blue means no hit)
  - 7 lighting multiplier
  - 8 temporal history

---

## 5. How it works (short version; details in `docs/ARCHITECTURE.md`)

```
DllMain: pin module, log, config, MinHook-detour d3d12!D3D12CreateDevice (or LdrRegisterDllNotification
         until d3d12.dll loads); register the ScriptHookV script + keyboard handler.
Game creates its device -> PatchFromDevice: patch device / queue (direct + compute) / list vtables
         (lists only if they implement ID3D12GraphicsCommandList7 on the same pointer).
Recording (game threads, hooks):
  CommandListTracker - root/heap/PSO state, render-target bindings (via DescriptorTracker), barriers
  BuildRaytracingAccelerationStructure -> TlasTracker clones the scene TLAS into RTSky's ring
  On binding close: FrameAnalyzer rules match ->
     Prepare   (end of G-buffer pass): depth -> linear depth + normals (RTSky textures)
     Composite (end of HDR lighting pass): sky LUTs + SH, DXR (or RayQuery) sky-visibility trace vs
               the TLAS clone, temporal + a-trous denoise, calibration probe, relight HDR target
     both recorded INTO the game's list under HookBypass, then RestoreState
ExecuteCommandLists hook (GPU order): FrameAnalyzer consumes binding logs; late-bind the newest
  GPU-ordered TLAS clone into the composite's descriptor; pairing check; GpuLifetime fence signal;
  publish TLAS clones.
Script thread (ScriptHookV): camera / clock / weather natives -> GameData (seqlock ring); hotkeys;
  status overlay (UI text natives); periodic status log.
```

**Key files:**

| File | What |
|---|---|
| `src/Render/Renderer.cpp` | The core: Prepare, Composite, resources, calibration gate, `OverlayLines` |
| `src/Track/FrameAnalyzer.cpp` | G-buffer phases and unique rules |
| `src/Track/CommandListTracker.cpp` | Per-list state, barriers, destruction watch |
| `src/Track/TlasTracker.cpp` | Clone ring, publish history, late binding |
| `src/Hooks/D3D12Hooks.cpp` | All hooks and installation |
| `src/Hooks/VTableHook.cpp` | Patching, adoption of copied vtables, verify |
| `src/Render/Barriers.cpp` | Legacy and enhanced barrier mirroring |
| `src/Render/GpuLifetime.cpp` | Per-queue fences |
| `src/Game/Overlay.cpp` | On-screen text |
| `src/Main.cpp` | Script loop and hotkeys |
| `shaders/RTSkyShared.h` | Constant-buffer layout and registers shared by C++ and HLSL (`FrameConstants` ≤ 1024 bytes, static-asserted) |

**Relighting math (CompositeCS):**

```
ratio = (E_keep + E_sky*R) / (E_keep + E_sky*A)
```

- `A` = `GameSkyOcclusion` (default 1).
- `R = S/U`. Both are denoised with the same weights. The near-field split is `U` = radiance surviving
  `NearFieldRadius` and `S` = radiance surviving the whole ray.
- The ratio is clamped to [`MinRatio`, `MaxRatio`] and blended in by `Strength`.
- Pixels with linear depth ≤ 0 are skipped. **If depth reconstruction fails everywhere, the composite
  is a silent no-op, debug views included.**

**Invariants that keep the game alive** (also in `CONTRIBUTING.md`):

1. Everything RTSky records runs under `HookBypass`.
2. Every injection restores the exact resource states and root/heap/PSO state.
3. No CPU waits on the GPU; objects stay alive through `Lifetime().Attach` plus fences.
4. Recording order is not GPU order: resolve at ExecuteCommandLists.
5. Never trace the game's live TLAS, only RTSky's clone, and never make one queue wait on another.
6. `RTSkyShared.h` is the single layout definition.

---

## 6. Diagnosing "no visible change" (the open problem)

The status overlay's **first line** walks the pipeline in order and names the first blocker. The same
line is logged every 10 s.

| What the user sees / the verdict says | Likely cause | What to do |
|---|---|---|
| **No status text at all** | ScriptHookV did not load RTSky, or RTSky disabled itself before registering the script | Read `RTSky.log`: "ScriptHookV is required", a missing export, or no log at all (ASI loader not loading `.asi`). Check that the ScriptHookV for Enhanced exports still match the decorated names in `src/Game/ScriptHookV.cpp` |
| `D3D12 hooks not installed` | Device created before RTSky loaded and the late path failed, or installation failed (`ID3D12GraphicsCommandList7` missing) | Log lines from `PatchFromDevice` / `InstallLate`. Streamline (`sl.interposer.dll`) proxies forward to the real objects, but verify on the real game |
| `looking for the G-buffer / lighting passes` | FrameAnalyzer never armed: G-buffer signature not found, HDR pass not found, rules not unique, or render targets created before the hooks | Use `Frame:` line plus the **frame dump**: pick `CompositeCandidate`, `GBufferOrdinal`, `CompositeOrdinal`. Turn the dump into a scenario in `tests/AnalyzerTests.cpp`. If the G-buffer is split across command lists at the same ordinal, see 7.1 |
| `not relighting - no scene TLAS captured` | Game RT off, or the game builds its TLAS through NVAPI (`NvAPI_D3D12_BuildRaytracingAccelerationStructureEx`), which RTSky does not hook | Check `TLAS: N builds seen`. If 0 with RT on, hook the NVAPI path (`nvapi64.dll` QueryInterface table) |
| `tracing, relighting HELD until calibration passes` | Camera / TLAS-space calibration below 0.35. **Most suspicious candidate.** Possible mismatches:<br>• near-clip / FOV / depth mode from natives vs the real projection<br>• TLAS in a space that is neither world nor camera-relative<br>• camera latency > 3 ticks | Look at `depth samples x/64` (0 means depth reconstruction is broken), the per-hypothesis scores, and debug view 6. **Num 4** relights anyway to see if anything is there. Fix ideas in section 8 |
| `not relighting - <reason>` | The renderer skipped the injection for the given reason, for example an unsupported state or a split barrier | Grep the reason in `Renderer.cpp` |
| `ACTIVE - relighting` but no visible difference | Relit the wrong pass (overwritten later), effect too subtle, or faded (`Faded out:` line for interior / cutscene / loading) | Num 3 split compare; debug views 1 and 7 (R and the multiplier); try `CompositeCandidate`; raise `Strength` / lower `MinRatio` |

Calibration details:

- The probe traces 64 rays per frame under 8 hypotheses: camera latency 0–3 × TLAS world or
  camera-relative.
- A ray "matches" when the hit distance is within 3% of the depth-buffer distance.
- Relighting starts when the chosen hypothesis scores ≥ `MinCalibrationScore`.
- `Latency` / `TlasSpace` can be pinned; the search then stays in the pinned row or column.

---

## 7. Known limitations and deliberate decisions

1. **G-buffer split across several command lists at the same ordinal** (GTA V Enhanced does this,
   ~15 lists): since v0.1.3 Prepare runs after every such binding when all of them lie inside the
   G-buffer phase, as one group per frame. Costs: one Prepare (a full-screen compute pass plus two
   depth transitions) per list, ~15 per frame. Groups are delimited by the Composite in recording
   order, so a game that records the next frame's G-buffer lists before this frame's lighting list
   would mix two frames in one group (the `paired late` counter shows it). If the rule also matches
   a depth-writing binding outside the phase, the analyzer still refuses to arm.
2. **Enhanced barriers ending in `NO_ACCESS`** are not taken over. The restore could not order the
   game's next access after RTSky's writes.
3. **TLAS built on async compute** that has not finished when the composite is submitted: the
   previous frame's clone is traced (at most one frame old). Same-queue or same-submission clones are
   current, through the late binding in `RendererImpl::OnSubmit`.
4. **Opacity micromaps**: only the DXR pipeline path on tier 1.2 traces them. The inline probe is
   disabled, so `Latency` / `TlasSpace` must be pinned.
5. **Device recreation or removal**: RTSky stops injecting for the session. There is no re-init.
6. **Lists that execute a bundle** are not injected into, because inherited root state is unknown.
   **Indirect draws with unknown command signatures** (created before the hooks) count as draws.
7. **Multi-subresource HDR targets** (mips or arrays) are skipped.
8. **Foliage alpha** is procedural (barycentric cells keyed by `InstanceID`), because the game's alpha
   textures are not reachable.
9. **Prepare/Composite pairing** is by recording order. If the game records lighting before its
   G-buffer list, relighting lags one frame. This is detected and counted (`late`), not fixed.

---

## 8. Unverified assumptions (verify against the real game first)

These were researched or derived but **could not be checked without the game**:

- **ScriptHookV for Enhanced** keeps the legacy decorated export names, and the native hashes are
  unchanged:
  - camera: `GET_FINAL_RENDERED_CAM_COORD` / `ROT` (order 2) / `FOV` / `NEAR_CLIP` / `FAR_CLIP`;
  - clock;
  - weather;
  - `IS_INTERIOR_SCENE`, `IS_CUTSCENE_ACTIVE`, `GET_IS_LOADING_SCREEN_ACTIVE`;
  - UI text: `SET_TEXT_*`, `BEGIN` / `END_TEXT_COMMAND_DISPLAY_TEXT`,
    `ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME`, `DRAW_RECT`.

  Hashes are in `src/Game/GameData.cpp` and `src/Game/Overlay.cpp`.
- **The camera natives match the rendered frame:**
  - FOV is **vertical** degrees;
  - near clip equals the projection near plane;
  - depth is reversed-Z (clear value 0). `DepthMode=auto` picks reversed-finite from the clear value;
    `reversed_infinite` and `standard` are options.
- **The camera basis** is `R = Rz(yaw)·Rx(pitch)·Ry(roll)` in Z-up world space (rotation order 2).
- **Sun orbit** from `time.xml`: `dc = π(0.5+(t−6)/14)`, `sun = (sin dc, −cos dc·cos 122°, −cos dc·sin 122°)`.
  `SunRoll`, `DayStartHour` and `DayLengthHours` are configurable.
- **The game builds its scene TLAS** with the standard `BuildRaytracingAccelerationStructure`, not NVAPI.
- **The G-buffer** is an MRT binding (≥ 3 RTs + depth) with the most draws. **The HDR lighting pass**
  is a later float-RT0 binding of the same size.

**Fix ideas if calibration is the blocker:**

- Have the probe estimate a **depth scale**: histogram log2(t_hit / t_depth) per hypothesis. A constant
  ratio means a near-plane mismatch; apply the scale in PrepareCS.
- Log the natives' near / far / FOV at startup.
- Add a hypothesis for a TLAS origin offset.
- Read the projection matrix from the game's constant buffers instead of natives. This is a bigger change.

---

## 9. History in brief

1. Research, design, all shaders, and the native host. DXC was built from source because the release
   download was blocked. The MinGW cross-build and the MSVC CI were set up.
2. The ReShade direction was rejected by the owner, and the project pivoted to the native `.asi`.
3. **Review round 1, D3D12 sync:**
   - TLAS clone published at submit;
   - exact enhanced-barrier scopes;
   - split / partial barriers;
   - UAV barriers;
   - per-frame LUTs.
4. **Review round 2**, covering hooks/threads, shaders/math, integration and robustness:
   - R2 sampling precision collapse (fixed-point sequence);
   - fp16 underflow at night;
   - immortal singletons (an exit crash);
   - safe vtable patching (`List7` check, adoption, write lock);
   - destruction watch;
   - module pin;
   - completed-only cross-queue TLAS;
   - device identity;
   - allocation backoff;
   - indirect-draw classification;
   - unique analyzer rules with discriminators;
   - G-buffer phases;
   - pairing checks;
   - calibration gate on the rendered hypothesis;
   - config sanitising.
5. **Review round 3**, re-review of the fixes:
   - TLAS publish history with slot generations;
   - compute-queue patching;
   - watch re-arm on reused addresses;
   - decal-safe Prepare (last depth-*writing* binding);
   - float-MRT passes don't split phases;
   - pinned calibration hysteresis;
   - **late TLAS binding at submission** (descriptor table, `DESCRIPTORS_VOLATILE`).
6. **Opened to contributors:** GPL-3.0, `CONTRIBUTING.md`, issue and PR templates, the release
   workflow, then v0.1.0-alpha.
7. The owner's first test showed no visible change. v0.1.1-alpha added numpad hotkeys, the status
   overlay, split compare and force relight.

---

## 10. Suggested next steps (in order)

1. **Get the owner's v0.1.1 report** and act on the verdict line using section 6.
2. If a frame dump arrives, turn it into an analyzer test scenario, then tune the detection defaults.
3. If calibration is the blocker, implement the depth-scale estimation (section 8), or pin values
   found with debug view 6.
4. When relighting is visible, tune the look defaults with the owner: `Strength`, `MinRatio`,
   `NearFieldRadius`, `ArtificialAmbient`, `GameSkyOcclusion`.
5. Performance pass: half-resolution trace option, fewer denoiser iterations.
6. Keep `docs/` and `config/RTSky.ini` in sync with behaviour changes. Bump the version, merge, and run
   the release workflow for each test build.
