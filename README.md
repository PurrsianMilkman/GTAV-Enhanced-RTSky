# RTSky: ray-traced sky lighting for GTA V Enhanced

> **Status: alpha, not yet tested in game.** The mod builds, and its math and frame-detection
> logic are covered by tests, but it was written without access to the game. The first in-game runs
> will need tuning. **Testers are very welcome:** see [Testing](#testing-and-reporting) and
> [Contributing](CONTRIBUTING.md).

GTA V Enhanced (the 2025 DirectX 12 PC edition) ray traces shadows, reflections, AO and global
illumination, but its **sky light is still faked**. The ambient term is a hemispherical colour
gradient from the timecycle multiplied by baked AO, so the sky never really casts shadows. RTSky
adds the missing piece: **world-space ray-traced sky lighting**. The sky casts soft, directional
shadows under bridges and overpasses, beneath tree canopies, in alleys and street canyons, under
cars and awnings, and inside garages and tunnel mouths.

RTSky is a **native mod**, not a ReShade/post-process shader:

* The rays are DXR rays traced in **world space** through **the game's own scene BVH**: the
  top-level acceleration structure Rockstar builds every frame for its RTGI and reflections. Off-screen
  and behind-camera geometry occludes the sky. Only the ray *start points* come from the screen,
  reconstructed from the depth buffer, exactly like the game's own RT passes.
* The shaders run **inside the game's frame**, recorded into the game's own D3D12 command lists. They
  run right after the deferred lighting pass and before fog, transparents, tonemapping, upscaling
  (DLSS/FSR) and the HUD.
* It is a single `RTSky.asi`, loaded by an ASI loader. It hooks D3D12 directly and does not need
  ReShade.

## How it works (short version)

1. **Capture**: RTSky hooks `BuildRaytracingAccelerationStructure` and clones the game's scene TLAS
   right after each build into its own buffer.
2. **Prepare** (end of the G-buffer pass): the game's depth buffer becomes linear depth and world normals.
3. **Sky model**: a physically based atmosphere (Hillaire 2020: transmittance, multiple scattering and
   sky-view LUTs) is driven by the game clock (sun orbit from `time.xml`) and the current weather. It is
   projected to spherical harmonics for the unoccluded sky irradiance.
4. **Trace** (DXR `DispatchRays`; a `RayQuery` compute path is optional): per pixel,
   cosine-distributed sky rays plus one soft sun/moon ray, against the game's TLAS. A miss sees the sky,
   a hit is occluded. Alpha-tested foliage gets stable stochastic coverage, because the game's alpha
   textures are not reachable. The hit-group table uses a stride of 0, so every game instance resolves
   to RTSky's own hit group.
5. **Denoise**: world-space temporal reprojection, then an SVGF-style à-trous filter. The shadowed and
   unshadowed sky estimates share weights (Heitz et al. 2018 ratio estimator).
6. **Composite**: the game's HDR lighting buffer is relit in place:
   `color × (E_keep + E_sky·R) / (E_keep + E_sky·A)`. Here `R` is the ray-traced sky visibility,
   `E_sky` the sky irradiance, and `E_keep` the light RTSky does not replace (sun, ground bounce,
   artificial light). Albedo cancels out, so no material data is needed.

[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) has the full design.

## Requirements

* GTA V Enhanced, **story mode only**. BattlEye must be off, so launch with `-nobattleye` or use the
  launcher toggle. Never use mods in GTA Online.
* **Ray tracing enabled in the game** (any of RT GI / RT reflections / RT shadows / RTAO), otherwise
  there is no scene BVH to trace. `Ray Tracing Scene BVH Quality = Very High` gives the most occluders.
* A DXR tier 1.1 GPU: NVIDIA RTX 20 series and newer, AMD RX 6000 and newer, Intel Arc.
* [ScriptHookV](http://www.dev-c.com/gtav/scripthookv/) for GTA V Enhanced, with its ASI loader
  (`xinput1_4.dll`). Ultimate ASI Loader also works. RTSky reads the camera, clock and weather
  through game natives.

## Download

Pre-built releases are on the [Releases page](https://github.com/PurrsianMilkman/GTAV-Enhanced-RTSky/releases):
`RTSky-<version>.zip` contains `RTSky.asi`, `RTSky.ini`, this README and the docs. Every push also
builds a zip as a CI artifact (Actions tab, needs a GitHub login).

## Install

1. Install ScriptHookV for Enhanced (`ScriptHookV.dll` and the ASI loader) into the game folder.
2. Copy `RTSky.asi` and `RTSky.ini` from the release zip into the same folder.
3. Start the game, load story mode, and look at `RTSky.log` next to the game executable.

On the first frames RTSky analyses the frame, finds the G-buffer and lighting passes, captures the
TLAS and calibrates the camera. Relighting starts once the calibration agrees with the depth buffer.
That takes about a second of camera movement.

## Hotkeys

NumLock must be on. Every key shows a short confirmation on screen, and all of them can be rebound
in the `[Hotkeys]` section of `RTSky.ini`.

| Key | Action |
|---|---|
| **Num 1** | RTSky on / off |
| Num 2 | on-screen status lines on / off |
| Num 3 | split-screen compare: left half original, right half RTSky |
| Num 4 | force relighting even while the camera calibration has not passed |
| Num 5 | next debug view |
| Num 6 | sun / moon shadow rays on / off |
| Num 7 | foliage mode: stochastic, opaque, ignored |
| Num 8 | near-field split on / off |
| Num 9 | trace path: DXR pipeline or inline RayQuery |
| Num 0 | reload `RTSky.ini` |
| Num . | write the next frame's structure to `RTSky_frame.log` |
| Num + / Num - | strength +0.1 / -0.1 |
| Num * | spatial denoiser on / off |
| Num / | reset the temporal history and the camera calibration |

The **status lines** in the top-left corner start with a verdict: what RTSky is doing, or the first
thing stopping it (no hooks, passes not found, no TLAS, calibration not passed, ...). Below it are
the details. If you see no status text at all, ScriptHookV did not load RTSky; check `RTSky.log`.

Debug views: 1 sky visibility, 2 sun visibility, 3 normals, 4 depth, 5 sky irradiance,
**6 TLAS alignment** (green means the game's BVH lines up with the screen, red means a mismatch, blue
means no geometry), 7 final lighting multiplier, 8 temporal history.

## Configuration

Every option is documented in [`config/RTSky.ini`](config/RTSky.ini). The ones you are most likely to touch:

* `Strength`, `MinRatio`: how strongly the sky shadows darken the image.
* `NearFieldRadius`: occluders closer than this are left to the game's own AO, so corners are not
  darkened twice. Use 0 if you turned the game's RTAO and SSAO off.
* `ArtificialAmbient`: the non-sky ambient level, which keeps nights and interiors from going black.
* `RaysPerPixel`, `Iterations`: quality and performance.
* `FoliageMode` / `FoliageOpacity`: how tree canopies occlude.

## Troubleshooting

See [docs/CALIBRATION.md](docs/CALIBRATION.md). In short:

* *"no G-buffer (MRT) pass seen yet"*: RTSky was loaded after the game created its render targets.
  Change the resolution or window mode once.
* *"no scene TLAS captured"*: the game's ray tracing is off.
* *Calibrating forever*: open debug view 6. If it is mostly red, try `DepthMode` / `FovScale` /
  `TlasSpace`, or send `RTSky.log` together with `RTSky_frame.log`.
* *Wrong pass relit* (fog or sky darkened, or nothing changes): use the frame dump and
  `CompositeCandidate`.
* *"not unique per frame"*: the G-buffer or lighting pass appears more than once per frame in a way
  RTSky cannot tell apart, so it stays off rather than relight the wrong pass. Send the frame dump.
* *"paired late"* counting up in the status line: the game records its lighting before its
  G-buffer, so the sky shadows lag one frame.
* *"the game uses opacity micromaps"*: set `Latency` and `TlasSpace` by hand (calibration cannot run).

## Limitations

* RTSky only sees what the game puts into its BVH. The RT range and LOD depend on *Scene BVH Quality*;
  beyond that range the sky counts as visible. TLAS builds made through NVAPI extensions would be invisible.
* The camera comes from ScriptHookV natives, a frame or two off the frame being rendered. The probe
  measures and compensates the latency, but very fast camera motion can show slight lag in the sky shadows.
* Foliage coverage is procedural, because the game's alpha textures are not accessible.
* The sun direction follows the `time.xml` orbit (CodeWalker's model). Weathers that override the
  light direction (snow, blizzard) can disagree slightly.
* Without a game to test against while writing it, the frame-structure detection is heuristic and
  self-checking. The frame dump exists to tune it on any game version.

## Testing and reporting

The most useful contribution right now is a test run:

1. Install as above, start story mode with ray tracing enabled, and play for a minute. Drive under a
   bridge, stand under trees, and turn the camera around.
2. Press **Num .** once during gameplay to write `RTSky_frame.log`, and take a screenshot of the
   status lines in the top-left corner.
3. Open an issue with the **In-game test report** template, attach `RTSky.log` and
   `RTSky_frame.log` (next to the game executable), and add screenshots with the split compare (Num 3)
   if you can.

The log's status line (written every 10 s) shows what RTSky found: the device, the G-buffer and
lighting passes, the TLAS, calibration, and why it skipped injection if it did.

## Building

Requirements: CMake 3.21+, a C++20 compiler (MSVC 2022 or MinGW-w64), and DXC (Windows SDK 10.0.22621+
or a standalone DXC). DirectX-Headers v1.619.5 is fetched automatically.

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
```

To cross-compile from Linux with MinGW-w64:

```sh
cmake -S . -B build-mingw -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake \
      -DCMAKE_BUILD_TYPE=Release -DRTSKY_DXC=/path/to/dxc
cmake --build build-mingw
```

All shaders are compiled to DXIL at build time and embedded in the `.asi`.

Host tests (no Windows SDK needed): the math tests and frame-analyzer scenarios, which run the real
tracker and analyzer sources against simulated frames.

```sh
cmake -S tests -B build-tests -DRTSKY_DIRECTX_HEADERS_DIR=/path/to/DirectX-Headers
cmake --build build-tests && ctest --test-dir build-tests --output-on-failure
```

## Contributing

Contributions are welcome. [CONTRIBUTING.md](CONTRIBUTING.md) explains the code layout, how to build
and test, and what needs work. [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) describes the design.

## License

RTSky is licensed under the [GNU General Public License v3.0](LICENSE). The vendored MinHook is
BSD-2-Clause (`third_party/minhook/LICENSE.txt`).

## Credits

* Atmosphere: S. Hillaire, *A Scalable and Production Ready Sky and Atmosphere Rendering Technique*,
  EGSR 2020 (reference implementation: UnrealEngineSkyAtmosphere).
* Denoising: Schied et al., *Spatiotemporal Variance-Guided Filtering* (2017). Ratio estimator: Heitz,
  Hill and McGuire, *Combining Analytic Direct Illumination and Stochastic Shadows* (2018).
* Sun orbit: the reverse-engineered model of `time.xml` used by CodeWalker.
* [MinHook](https://github.com/TsudaKageyu/minhook) (BSD-2-Clause), vendored in `third_party/minhook`.
* [DirectX-Headers](https://github.com/microsoft/DirectX-Headers) (MIT).
