# Calibration and troubleshooting

RTSky has to discover several facts about the running game that are not documented anywhere. It
measures them at run time and logs every decision to `RTSky.log`. Every 10 seconds the log gets a status line:

```
Status: renderer ready, 1520 prepares, 1518 composites, calibration: latency 1, TLAS world, scores
[world/camera]: L0 0.12/0.00 L1 0.83/0.01 L2 0.40/0.00 L3 0.18/0.00 | analyzer: armed
```

## 1. Device and hooks

| Log line | Meaning |
|---|---|
| `D3D12CreateDevice detoured` / `Game created its D3D12 device` | early path, everything is visible from the start |
| `using the late hook path` | RTSky was loaded after the game's device existed. Render targets created before that are unknown. Change the resolution or window mode once. |
| `ID3D12GraphicsCommandList7 is not available` | the D3D12 runtime is too old. Update Windows and the GPU driver. |

## 2. Frame structure (`analyzer:` in the status line)

RTSky works out the frame from the order the game submits its work in:

* **G-buffer**: the multi-render-target pass (3 or more targets plus depth) with the most draws. If the
  game binds it several times (opaque, decals, foliage), splits it across command lists or suspends
  its render pass, all those bindings form one *G-buffer phase*, and RTSky reads the depth buffer at
  the end of the phase's **last** binding, where the depth is complete and its state is certain.
* **HDR lighting**: the `CompositeCandidate`-th pass after the G-buffer that renders into a float
  target of the same size. RTSky relights this target when the pass ends, before fog, sky,
  transparents and post-processing.

Press **Num .** to write one frame of submitted passes to `RTSky_frame.log`, in GPU order:

```
---- list 0000020F... (queue 0000020E...): 2311 draws, 1 TLAS builds, 140 BLAS builds, 0 DispatchRays
  #3   4 RT [RGBA8_UNORM,RGBA8_UNORM,RGBA8_UNORM,RGBA8_UNORM] + D32S8 2560x1440   draws 1890  mrt#0 dsv clear=0.00   <== PREPARE
  #4   1 RT [RGBA16F] + D32S8 2560x1440                                           draws 12    hdr#0 dsv ro-depth    <== COMPOSITE
  #5   1 RT [RGBA16F] + D32S8 2560x1440                                           draws 230   hdr#1 dsv ro-depth
```

The rules RTSky matches at recording time are "this signature at this ordinal within its command
list" (`mrt#n` / `hdr#n`). A rule is only armed when it matches **exactly one** pass per frame. If the
same signature appears at the same ordinal in two lists, the dump's `(after N mrt)` / `(after N hdr)`
counts, the number of G-buffer or HDR passes recorded earlier in the same list, are used to tell
them apart. If that is still ambiguous, the status says `not unique per frame` and nothing is injected.
Pin an ordinal that is unique in the dump.

If the image looks wrong, for example the sky dome, fog or water is darkened (relit too late), or
nothing changes (relit too early, before the ambient term is added), pick another candidate with
`CompositeCandidate=n`. `CompositeOrdinal` / `GBufferOrdinal` pin the per-list ordinals directly.

**Pairing.** The Composite uses the camera and depth of the Prepare recorded before it. A game that
records its lighting list before the frame's G-buffer list (parallel recording) relights with the
previous frame's depth, one frame late, which can show as thin halos while turning. The status line
counts these (`paired late`), and the log warns once.

`frame structure changed, re-analysing` is normal in menus, loading screens and cutscene transitions.

## 3. Scene BVH

* `no scene TLAS captured (enable the game's ray tracing)`: no top-level BVH build was seen. Enable
  any RT effect.
* `the scene TLAS was not rebuilt`: RTSky never traces the same BVH twice in a row, because the
  geometry it references may already be freed. It traces the newest BVH that is guaranteed to have
  been built before its trace on the GPU. That is normally the current frame's, bound when the game
  submits the frame (`TLAS bound at submit` in the status line). With a TLAS built on async compute
  it can be the previous frame's.
* `the game uses opacity micromaps`: only the DXR pipeline path on a tier 1.2 GPU can trace them. The
  calibration probe uses inline ray queries, which cannot, so it is disabled: set `Latency` and
  `TlasSpace` explicitly (section 4) to enable relighting.
* RTSky always traces its own copy of the BVH. If that copy cannot be allocated (video memory full),
  the frame is skipped and the allocation is retried about every 2 seconds.
* `TlasSelect=n` picks one of several BVHs if the game builds more than one per frame. The frame dump
  shows the build counts per command list.

## 4. Camera calibration

Camera, FOV and near/far come from ScriptHookV on the game's script thread. Two things are measured,
not assumed:

* **latency**: how many script ticks the camera sample lags or leads the frame being rendered;
* **TLAS space**: whether the game's BVH is in world space or relative to the camera.

The probe traces 64 primary rays per frame through the game's BVH under 8 hypotheses. It counts how
often the hit distance matches the depth buffer within 3%, and the best hypothesis wins, with hysteresis.
Latency can only be told apart while the camera moves, so move or turn the camera for a few seconds
after loading.

Relighting starts once the best score reaches `MinCalibrationScore` (0.35). Scores of 0.6 to 0.9 are
typical; foliage and level-of-detail differences between the raster and RT geometry account for the rest.

If every score stays low:

1. Open **debug view 6** (Num 5 six times). Green pixels are where the BVH lines up with the screen.
   **Num 4** (force relight) shows the relighting anyway, aligned or not.
2. Everything red with a constant offset suggests a wrong `TlasSpace`. Red that grows with distance
   suggests a wrong `DepthMode` (try `reversed_infinite` or `standard`). Red that grows towards the
   screen edges suggests a wrong FOV (`FovScale`).
3. Pin what you found (`Latency=1`, `TlasSpace=world`, ...) and set `CalibrationProbe=0`.

With only one of the two pinned, the probe still picks the other one, within the pinned row, and the
`MinCalibrationScore` gate applies to the combination actually used.

## 5. Look tuning

| Symptom | Try |
|---|---|
| everything under cover is too dark | raise `MinRatio`, `ArtificialAmbient`, or lower `Strength` |
| corners and contact areas too dark (AO counted twice) | raise `NearFieldRadius` (1 to 2 m) |
| flat ground shadows itself (blotches) | raise `NormalBias` / `DistanceBias` |
| noisy or crawling shadows | `RaysPerPixel=2`, `Iterations=5`, raise `MaxHistory` |
| ghosting behind moving cars | lower `MaxHistory`, raise `DepthReject` strictness (lower value) |
| tree shadows too solid or too weak | `FoliageOpacity`, or `FoliageMode=opaque`/`ignore` |
| night or interiors too dark | `ArtificialAmbient`, `InteriorStrength=0` |
| shadows lag the camera | pin `Latency` to the value that scores best |
