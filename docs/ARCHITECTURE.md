# RTSky architecture

RTSky adds **ray-traced sky lighting** to GTA V Enhanced (the DirectX 12 PC edition). For every
on-screen surface it traces world-space rays through the game's own DXR scene (the top-level
acceleration structure (TLAS) that Rockstar builds each frame for RTGI and reflections). Rays that
escape to the sky pick up physically based sky radiance. Rays that hit geometry are occluded. The result
is soft, directional sky shadowing: under bridges, overhangs, trees, in alleys, and between cars and
buildings. The game's own ambient term fakes this. RTSky composites the result into the game's
HDR lighting buffer **inside the game's frame**, before post-processing, tonemapping, upscaling and UI.

It is native code: a single `RTSky.asi` loaded by the ScriptHookV ASI loader. It hooks D3D12 and DXGI
directly (vtable patching). It does not use ReShade or any other post-processing injector.

This document is the implementation contract. File names, types and function names referenced
here exist in the source tree.

---

## 1. Frame data flow

```
 game recording threads                          RTSky
 ─────────────────────                           ─────
 BuildRaytracingAccelerationStructure(TLAS) ──►  TlasTracker   remembers dest VA + instance count
 OMSetRenderTargets / BeginRenderPass       ──►  CommandListTracker: current RTVs/DSV, viewport,
 ResourceBarrier / Barrier                  ──►                     observed states/layouts,
 Draw* / ExecuteIndirect                    ──►                     draw count since bind,
 Set*Root* / SetPipelineState* / heaps      ──►                     full root state (for restore)
 CreateRenderTargetView / DSV / SRV         ──►  DescriptorTracker: CPU handle -> resource + format

 switch-away from the "HDR lighting" binding ──► Injector (on the game's DIRECT command list):
      (identified by FrameAnalyzer)                1. depth plane -> SRV state (barrier)
                                                   2. PrepareCS: linear depth + normals
                                                   3. atmosphere LUTs / SH (when dirty)
                                                   4. DXR DispatchRays  (or inline RayQuery CS)
                                                   5. TemporalCS  6. ATrousCS x N
                                                   7. CompositeCS into the HDR target
                                                   8. restore every barrier + all root state

 ExecuteCommandLists(DIRECT)                ──►  FrameAnalyzer consumes binding logs in execution
                                                  order; Renderer signals its fence after lists
                                                  that contained an injection
 Present                                    ──►  frame epoch++, analyzer decides next frame's rule,
                                                  hotkeys, deferred releases
 ScriptHookV script tick (game thread)      ──►  GameData: camera ring buffer, clock, weather
```

The injection happens **between** two of the game's own commands, on the game's own command list.
Everything RTSky binds is restored before control returns to the game. RTSky never blocks a
game thread on the GPU: if a resource it needs is still in flight, that frame's injection is skipped.

## 2. Source layout

```
CMakeLists.txt                 builds RTSky.asi, compiles shaders with DXC and embeds them
cmake/EmbedFile.cmake          binary -> C header
cmake/toolchain-mingw64.cmake  optional MinGW cross build (used for CI / syntax checks)
config/RTSky.ini               default, fully commented configuration
shaders/RTSkyShared.h          C++/HLSL shared constant layout, bindings, enums
shaders/*.hlsli, *.hlsl        see §6
src/Main.cpp                   DllMain, init thread, ScriptHookV registration, shutdown
src/Common/                    Log, Config (INI), Math (float3/float4x4), ComPtr, Hash
src/Hooks/VTableHook.*         generic vtable patching with per-vtable original tables
src/Hooks/VTableIndices.c      C translation unit: offsetof(<Iface>Vtbl, Method) / sizeof(void*)
src/Hooks/D3D12Hooks.*         hook bodies; forward to trackers / injector; bypass logic
src/Track/CommandListTracker.* per-command-list recorded state (+ save/restore)
src/Track/DescriptorTracker.*  RTV/DSV/SRV(AS) CPU-descriptor -> view info
src/Track/TlasTracker.*        top-level AS builds
src/Track/FrameAnalyzer.*      identifies depth / G-buffer / HDR target / injection binding
src/Render/Renderer.*          all GPU resources, pipelines and the injected pass sequence
src/Render/RtPipeline.*        DXR state object + shader tables
src/Render/Barriers.*          legacy/enhanced barrier helpers for game-owned resources
src/Game/ScriptHookV.*         dynamic binding to ScriptHookV.dll exports
src/Game/GameData.*            natives -> camera ring buffer, clock, weather, flags
src/Game/SunModel.*            clock -> sun/moon directions
src/Game/Weather.*             weather hash -> cloudiness/brightness parameters
```

## 3. Hooking layer (`src/Hooks`)

### 3.1 Getting the vtables
`Main.cpp` starts an init thread from `DllMain`. D3D work inside `DllMain` is forbidden because it
runs under the loader lock. The init thread:

1. waits until `d3d12.dll` and `dxgi.dll` are loaded (poll `GetModuleHandleW`, 50 ms, up to 60 s);
2. creates a throw-away `ID3D12Device` (default adapter, FL 12_0), a DIRECT queue, an allocator, a
   DIRECT `ID3D12GraphicsCommandList` and a hidden 1x1 window with a flip-model swapchain
   (`IDXGIFactory2::CreateSwapChainForHwnd`, 2 buffers, `DXGI_FORMAT_R8G8B8A8_UNORM`);
3. reads the vtable pointer of each object and patches the entries listed below;
4. releases the dummies and destroys the window.

Dummy objects are created *through the same d3d12.dll / D3D12Core* as the game, so their classes and
therefore their vtables are the game's (the Agility SDK redirection is process-wide). NVIDIA Streamline
and overlays wrap the game-facing objects, but forward to the real runtime objects, whose vtables are
the patched ones.

`VTableHook` also supports **lazy patching**. `ExecuteCommandLists` checks the vtable pointer of every
submitted list. If an unseen vtable appears (debug layer, a different runtime class), it is patched
too. Each patched vtable owns its own table of originals. A hook body fetches its original with
`VTableHook::Original<Fn>(thisPtr, index)`. That reads `*(void***)thisPtr` and finds the saved table,
with a single-entry fast path.

### 3.2 Hooked methods and indices
Indices are **not hand-written**: `VTableIndices.c` is compiled as C with `COBJMACROS`/C interface
definitions, and exports `const unsigned RTSKY_IDX_<Iface>_<Method> = offsetof(<Iface>Vtbl, Method) / sizeof(void*);`
It uses the newest interface that contains the method (`ID3D12GraphicsCommandList7Vtbl`,
`ID3D12Device5Vtbl`, `ID3D12CommandQueueVtbl`, `IDXGISwapChain3Vtbl`). Reference values (latest
DirectX-Headers): CL `Close 9, Reset 10, DrawInstanced 12, DrawIndexedInstanced 13, Dispatch 14,
SetPipelineState 25, ResourceBarrier 26, SetDescriptorHeaps 28, SetComputeRootSignature 29,
SetGraphicsRootSignature 30, SetComputeRootDescriptorTable 31, SetGraphicsRootDescriptorTable 32,
SetComputeRoot32BitConstant 33, SetGraphicsRoot32BitConstant 34, SetComputeRoot32BitConstants 35,
SetGraphicsRoot32BitConstants 36, SetComputeRootConstantBufferView 37, SetGraphicsRootConstantBufferView 38,
SetComputeRootShaderResourceView 39, SetGraphicsRootShaderResourceView 40,
SetComputeRootUnorderedAccessView 41, SetGraphicsRootUnorderedAccessView 42, RSSetViewports 21,
OMSetRenderTargets 46, ClearDepthStencilView 47, ClearRenderTargetView 48, ExecuteIndirect 59,
BeginRenderPass 68, EndRenderPass 69, BuildRaytracingAccelerationStructure 72, SetPipelineState1 75,
DispatchRays 76, Barrier 80`; Device `CreateShaderResourceView 18, CreateRenderTargetView 20,
CreateDepthStencilView 21`; Queue `ExecuteCommandLists 10`; SwapChain `Present 8, ResizeBuffers 13,
Present1 22, ResizeBuffers1 39`.

| Object | Methods hooked | Purpose |
|---|---|---|
| Device | CreateRenderTargetView, CreateDepthStencilView, CreateShaderResourceView | descriptor → resource/format/flags |
| Command list | Reset, Close | per-list state lifetime |
| | OMSetRenderTargets, BeginRenderPass, EndRenderPass | binding log, **injection trigger** |
| | RSSetViewports | viewport of the depth / HDR passes (dynamic resolution) |
| | ResourceBarrier, Barrier | observed state / layout of tracked resources |
| | DrawInstanced, DrawIndexedInstanced, ExecuteIndirect | draw counts per binding |
| | ClearDepthStencilView | known DEPTH_WRITE anchor |
| | SetDescriptorHeaps, Set{Compute,Graphics}RootSignature, all Set{Compute,Graphics}Root* , SetPipelineState, SetPipelineState1 | state to restore after injection |
| | BuildRaytracingAccelerationStructure | TLAS capture |
| | DispatchRays | statistics (frame dump), TLAS-consumer detection |
| Queue | ExecuteCommandLists | execution order, lazy vtable patch, fence signal |
| Swapchain | Present, Present1, ResizeBuffers, ResizeBuffers1 | frame epoch, hotkeys, device acquisition, resize flush |

### 3.3 Bypass
RTSky's own D3D calls go through the patched vtables too. `thread_local int g_bypass` is incremented
by `HookBypass` (RAII) around every RTSky-issued D3D call sequence. Every hook body starts with
`if (g_bypass) return Original(...)(args...);`. Nothing RTSky does is ever tracked as game state.

### 3.4 Failure policy
Any unexpected condition (failed HRESULT, missing tier, unknown formats, analyzer not confident)
disables the injection. The game keeps rendering untouched, and the reason goes to `RTSky.log`.
Hooks themselves never throw and never allocate on hot paths except when a new command list is
first seen.

## 4. Tracking (`src/Track`)

### 4.1 DescriptorTracker
Sharded hash map (16 shards, `SRWLOCK`) keyed by `D3D12_CPU_DESCRIPTOR_HANDLE.ptr`:
```cpp
struct ViewInfo {
  ID3D12Resource* resource;       // not AddRef'd
  DXGI_FORMAT viewFormat;
  DXGI_FORMAT resourceFormat;     // resource->GetDesc() at creation time
  UINT64 width; UINT height; UINT16 depthOrArraySize; UINT16 mipLevels;
  D3D12_RESOURCE_FLAGS resourceFlags;
  enum Kind : uint8_t { RTV, DSV, SRV_AS } kind;
  D3D12_DSV_FLAGS dsvFlags;       // DSV only
  D3D12_GPU_VIRTUAL_ADDRESS asLocation; // SRV_AS only
};
```
Only RTVs, DSVs and acceleration-structure SRVs are recorded. Other SRVs are ignored for performance.

### 4.2 CommandListTracker
One `ListState` per `ID3D12GraphicsCommandList*`, created on first sight, reset on `Reset`.
A `thread_local` single-entry cache (`list → ListState*`) avoids the map lookup on hot paths.
Recording a list is single-threaded by D3D12 rules, so `ListState` needs no lock.

`ListState` holds:
* `type` (from `GetType()`), `frameEpoch` at Reset;
* **root state**: descriptor heaps (≤2), compute & graphics root signature, and per root parameter
  index (≤64) one of {descriptor table GPU handle, root CBV/SRV/UAV VA, 32-bit constants blob};
  current pipeline: either a PSO (`SetPipelineState`) or a state object (`SetPipelineState1`),
  whichever was set last; `Reset(alloc, initialPSO)` sets PSO = initialPSO and clears everything else.
  Setting a root signature clears that bind point's root parameters (D3D12 semantics);
* **binding**: current RTV handles (≤8) + resolved `ViewInfo`s, DSV handle + info, viewport[0],
  `drawsSinceBind`, `inRenderPass`;
* **observed resource states**: small vector `{ID3D12Resource*, subresource, legacyState | layout,
  isEnhanced, seq}` updated from `ResourceBarrier` / `Barrier` for any resource (last write wins);
* **binding log** for the analyzer: vector of `BindingRecord { rtv[8] resources/formats/dims,
  dsv resource/format/flags, viewport, draws, listOrdinal }`, closed when the binding changes.

`void RestoreState(ID3D12GraphicsCommandList* cl, const ListState&)`: re-applies heaps, both root
signatures and every recorded root argument, then the last pipeline (PSO or state object). The
injector always calls it before it returns, whatever succeeded or failed in between.

### 4.3 TlasTracker
On `BuildRaytracingAccelerationStructure` with `Inputs.Type == TOP_LEVEL` it records
`{DestAccelerationStructureData, NumDescs, Flags, epoch, sequence}` in a ring of 64. It also keeps
`maxInstances` over the last 120 epochs. `bool GetSceneTlas(uint64_t epochNow, TlasInfo* out)`
returns the most recent build whose `NumDescs >= 0.5 * maxInstances` and `epoch >= epochNow - 1`
(config `TlasSelect=auto`), or the n-th distinct VA (config `TlasSelect=<n>`). A TLAS that was not
rebuilt in the last 2 epochs is never used, because its memory may already be freed.
Bottom-level builds are counted for the frame dump only.

### 4.4 FrameAnalyzer
`OnExecute(queue, lists)` appends each list's binding log to the current epoch in **execution order**.
`OnPresent()` analyses the finished epoch:

* **main depth**: the DSV resource with the most draws over bindings whose DSV width/height ≥ 50% of
  the swapchain. Hysteresis: it must win 3 epochs in a row to replace the current choice.
* **G-buffer binding**: the binding with the most draws among those with ≥3 RTVs and DSV == main depth.
* **HDR target**: the first binding **after** the G-buffer binding (execution order) whose RTV0 has a
  float format (`R16G16B16A16_FLOAT`, `R11G11B10_FLOAT`, `R32G32B32A32_FLOAT`, `R9G9B9E5` excluded)
  and the same dimensions as the main depth. `CompositeBindingIndex=n` in the INI picks the n-th candidate.
* **injection rule**: `{ rtv0 resource, dsv resource (may be null), rtvCount, occurrence }`.
  `occurrence` is the ordinal of this `{rtv0, dsv}` pair among the epoch's bindings. The rule must
  be stable for 3 epochs before it is armed.

`bool ShouldInject(const ListState&, const BindingRecord& closing)` is called when a binding closes
(the next `OMSetRenderTargets`, or `EndRenderPass`). It returns true when the rule is armed, the
closing record matches it, `draws > 0`, the list is DIRECT, and no injection has yet been recorded
for the current `{rtv0, occurrence}` in this epoch (atomic).

`DumpFrame()` (hotkey) writes the next complete epoch to `RTSky_frame.log`: every binding, clear,
barrier on tracked resources, TLAS/BLAS build and DispatchRays, in execution order with formats and
dimensions. This is the tool for tuning the rule on other game versions.

## 5. Renderer (`src/Render`)

### 5.1 Capabilities
On first use: `ID3D12Device5` required; `D3D12_FEATURE_D3D12_OPTIONS5.RaytracingTier >= 1_1`
(RayQuery is used by the probe; the state-object path works at 1_0 but the mod requires 1_1).
Typed UAV loads for the HDR format are checked with `CheckFeatureSupport(FORMAT_SUPPORT)`.
`D3D12_FEATURE_D3D12_OPTIONS12.EnhancedBarriersSupported` is recorded.

### 5.2 Descriptor heap
One shader-visible `CBV_SRV_UAV` heap with 256 descriptors owned by RTSky. It is written once per
(re)creation of resources, plus 2 per-injection slots for the game-owned depth SRV and HDR target UAV,
which are re-created only when the game resource changes. Every unused slot holds a null descriptor.
Each pass has a fixed **SRV table** (8 slots) and **UAV table** (8 slots), laid out contiguously
(`PassTables[pass]`). See `RTSkyShared.h` for the per-pass register assignments.

### 5.3 Root signature (shared by every compute pass and as DXR global root signature)
| # | Type | Register | Content |
|---|---|---|---|
| 0 | root CBV | b0 | `FrameConstants` (upload ring, 256-byte aligned slots) |
| 1 | root SRV | t0, space1 | scene TLAS (game VA) |
| 2 | 32-bit constants ×8 | b1 | `PassConstants` |
| 3 | descriptor table SRV×8 | t0–t7, space0 | per pass |
| 4 | descriptor table UAV×8 | u0–u7, space0 | per pass |
| static | sampler | s0 linear clamp, s1 point clamp | |

Root signature version 1.1, ranges `DESCRIPTORS_VOLATILE | DATA_VOLATILE`.

### 5.4 Resources (trace resolution = viewport of the depth pass, W×H)
| Name | Format | Notes |
|---|---|---|
| `LinearDepth[2]` | R32_FLOAT | view-space distance along forward axis, ping-pong (cur/prev) |
| `Normal[2]` | R16G16_SNORM | octahedral world normal |
| `TraceS` | R16G16B16A16_FLOAT | rgb = shadowed sky sample S, a = sun/moon visibility |
| `TraceU` | R16G16B16A16_FLOAT | rgb = unshadowed sky sample U, a = mean hit fraction |
| `HistS[2]`,`HistU[2]` | R16G16B16A16_FLOAT | temporal accumulation |
| `HistMeta[2]` | R16G16B16A16_FLOAT | x = history length, y = E[lum S], z = E[lum S²], w = variance |
| `FiltS[2]`,`FiltU[2]` | R16G16B16A16_FLOAT | à-trous ping-pong (variance is carried in FiltU.a) |
| `SceneCopy` | HDR target format (typeless → typed UAV) | only if the HDR target lacks `ALLOW_UNORDERED_ACCESS` |
| `TransmittanceLut` | R16G16B16A16_FLOAT 256×64 | |
| `MultiScatterLut` | R16G16B16A16_FLOAT 32×32 | |
| `SkyViewLut` | R16G16B16A16_FLOAT 192×108 | per frame |
| `SkyData` | structured buffer, `SkyData` (see shared header) | SH9 sky irradiance, sun/moon irradiance |
| `ProbeResults` + readback | uint buffer 64 | calibration probe |
| `FrameConstants` ring | upload, 16 × 256 B aligned | fenced |
| shader tables | upload | raygen / miss / hitgroup (hit group stride 0) |

All textures are created in `COMMON` and kept in a known state. RTSky tracks its own resources'
states exactly.

### 5.5 Pass sequence (inside the injection)
1. **Depth to SRV**. The game depth's plane-0 subresource goes to `NON_PIXEL_SHADER_RESOURCE`
   (legacy) or `LAYOUT_SHADER_RESOURCE` (enhanced), starting from the observed or inferred state (§5.6).
2. **PrepareCS** (8×8): game depth → `LinearDepth[cur]`, `Normal[cur]`. Normals are reconstructed from
   depth: for each axis take the neighbour with the smaller depth discontinuity, then use the cross product.
3. Depth back to its original state.
4. **Atmosphere** (only when dirty or every frame for sky view): `TransmittanceCS` and `MultiScatterCS`
   run once. `SkyViewCS` runs every frame. `SkyProjectCS` (1 group of 256) projects sky radiance
   (upper hemisphere only) to SH9 and computes sun and moon irradiance at the camera.
5. **Trace**: `DispatchRays(W, H)` with the state object (`RaysPerPixel` cosine-distributed sky rays +
   1 sun/moon cone ray), or `RTSkyInlineCS` (RayQuery) with `TracePath=inline`.
6. **TemporalCS**: reprojects with the previous camera; bilinear taps are validated by depth and normal.
   It accumulates S, U, the luminance moments and the history length.
7. **ATrousCS** × `DenoiseIterations` (step 1, 2, 4, 8): 5×5 B3 kernel with plane-distance, normal and
   variance-guided luminance edge stops. S and U are filtered **with the same weights**.
8. **HDR target to UAV** (in place when it allows UAV; otherwise copy it to `SceneCopy`, composite
   there, and copy back).
9. **CompositeCS**: the relighting ratio (§7.6) or a debug view, written into the HDR target.
10. HDR target back to `RENDER_TARGET`, then `RestoreState`.

Temporal ping-pong indices advance once per executed injection.

### 5.6 Knowing the state of game-owned resources
At the injection point:
* The RTVs of the closing binding were drawn to (`draws > 0`), so they were in `RENDER_TARGET` /
  `LAYOUT_RENDER_TARGET` at the last draw.
* The DSV's state follows from the DSV's flags: `READ_ONLY_DEPTH` → `DEPTH_READ`, otherwise `DEPTH_WRITE`.
  Enhanced layout: `DEPTH_STENCIL_READ` / `DEPTH_STENCIL_WRITE`.
* Barriers on those resources *after the last draw* on this list override the inference (`ListState`
  observed states carry a sequence number).
* Whether a resource uses legacy states or enhanced layouts is decided by the last barrier API the
  game used on it (global map, updated by both hooks). With no observation, use legacy states.

Only plane 0 (subresource 0) of the depth resource is transitioned. RTSky transitions back to exactly
the state it found.

### 5.7 Fences and lifetimes
The renderer owns an `ID3D12Fence`. When `ExecuteCommandLists` submits a list that contains an
injection, the hook forwards the call and then signals `fence = ++value` on that queue. The upload
ring slot and history indices used by that injection are tagged with the value. A slot is reused only
if `fence->GetCompletedValue() >=` its tag; otherwise the injection is skipped (never waits).
Resources replaced on resize go to a deferred-release list keyed by fence value.

## 6. Shaders (`shaders/`)

All shaders include `RTSkyShared.h` (C++/HLSL shared) and `Common.hlsli`.

| File | Entry / target | Purpose |
|---|---|---|
| `AtmosphereCS.hlsl` | `TransmittanceCS`, `MultiScatterCS`, `SkyViewCS` / cs_6_0 | Hillaire 2020 LUTs |
| `SkyProjectCS.hlsl` | `SkyProjectCS` / cs_6_0 | SH9 of upper-hemisphere sky radiance, sun/moon irradiance |
| `PrepareCS.hlsl` | `PrepareCS` / cs_6_0 | linear depth + reconstructed normals |
| `RTSkyLib.hlsl` | `RayGen`, `Miss`, `AnyHit` / lib_6_3 | DXR pipeline |
| `RTSkyInlineCS.hlsl` | `TraceInlineCS` / cs_6_5 | RayQuery variant |
| `TemporalCS.hlsl` | `TemporalCS` / cs_6_0 | reprojection + accumulation |
| `ATrousCS.hlsl` | `ATrousCS` / cs_6_0 | spatial filter |
| `CompositeCS.hlsl` | `CompositeCS` / cs_6_0 | relighting + debug views |
| `ProbeCS.hlsl` | `ProbeCS` / cs_6_5 | camera / TLAS-space calibration |

`TraceCommon.hlsli` contains everything the two trace paths share: sample generation, origin offset,
sky radiance lookup, foliage mask and output packing. The only difference between the paths is the
`TraceVisibility()` implementation.

## 7. Algorithms

### 7.1 Coordinate conventions
GTA V world space is right-handed and Z-up (X east, Y north), in metres. The camera looks down its
local +Y, with local +X right and +Z up. `GET_FINAL_RENDERED_CAM_ROT(2)` returns degrees
(x = pitch, y = roll, z = yaw), and the world rotation is `R = Rz(yaw) · Rx(pitch) · Ry(roll)`, so
`forward = (-sin(yaw)cos(pitch), cos(yaw)cos(pitch), sin(pitch))`. Vertical FOV comes from
`GET_FINAL_RENDERED_CAM_FOV` in degrees; aspect = viewport width / height.

Shaders never build 4×4 matrices. `FrameConstants` carries `camRight*tanHalfFovX`,
`camUp*tanHalfFovY`, `camForward`, and the camera position split into a high part (float) and a low
part. For pixel uv:
`ndc = (2u-1, 1-2v)`, `rayDir = forward + ndc.x*right' + ndc.y*up'` (not normalised),
`posRel = rayDir * viewZ` (camera-relative world offset).

Depth → viewZ for reversed-Z: finite `viewZ = n·f / (d·(f−n) + n)`, infinite `viewZ = n / d`;
standard Z: `viewZ = n·f / (f − d·(f−n))`. `DepthMode` in the INI (default reversed-finite). Sky
pixels (`d == 0` for reversed-Z) are marked invalid.

**TLAS space**: `tlasPos = posRel + (camPos − tlasOrigin)`, where `tlasOrigin = 0` (world, default)
or `camPos` (camera-relative). The calibration probe (§7.8) selects this and the camera latency.

### 7.2 Sky radiance model
Hillaire 2020 (reference implementation *UnrealEngineSkyAtmosphere*), Earth defaults: ground radius
6360 km, top 6460 km, Rayleigh scattering (5.802, 13.558, 33.1)e-3 /km with scale height 8 km, Mie
scattering 3.996e-3 /km and absorption 4.4e-3 /km with scale height 1.2 km and g = 0.8, ozone absorption
(0.650, 1.881, 0.085)e-3 /km as a tent profile centred at 25 km, 30 km wide, ground albedo 0.3.
Transmittance LUT 256×64 (Bruneton mapping), multiple scattering LUT 32×32 (Ψ_ms, 64 directions), and
sky-view LUT 192×108 with non-linear latitude mapping. The camera altitude is world Z (clamped to 0 or
above) + 1 m.

`SkyRadiance(dir)` = `SkyViewLut(dir) · SunIlluminance` (+ the moon term when the moon is the dominant
light) blended toward a CIE overcast distribution `L_oc(θ) = L_z·(1 + 2cosθ)/3` by the weather
cloudiness `c`. `L_z` is set so that the overcast sky's horizontal irradiance equals
`OvercastTransmission(c) · (E_sun,h + E_sky,h,clear)`. Directions below the horizon return 0:
sky light is by definition from the upper hemisphere, and ground bounce is left to the game's RTGI.

### 7.3 SH projection
`SkyProjectCS` evaluates `SkyRadiance` on 1024 Fibonacci directions of the upper hemisphere
(weight 2π/1024) and projects onto real SH L2. It stores `c_lm · Â_l` (Â0 = π, Â1 = 2π/3, Â2 = π/4),
so that `E(n) = Σ c'_lm Y_lm(n)` is the irradiance of the unoccluded sky on a surface with normal n.
It also stores `E_sun` (solar illuminance × transmittance to the camera × (1 − c)²) and `E_moon`.

### 7.4 Trace
Per valid pixel, with blue-noise-like sampling (R2 sequence + interleaved gradient noise, rotated per frame):
```
origin  = tlasPos + N·(NormalBias + DistanceBias·viewZ)
for i < RaysPerPixel:
    dir = CosineSampleHemisphere(N, u_i)
    L   = dir.z > 0 ? SkyRadiance(dir) : 0
    V   = L != 0 ? TraceVisibility(origin, dir, tMin, MaxRayDistance) : 0
    S  += L·V ;  U += L
S /= n ; U /= n
light = sun if sun.z > −0.05 else moon
V_sun = TraceVisibility(origin, ConeSample(light.dir, light.angularRadius·Softness, u), …)
```
`TraceVisibility` uses `RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | SKIP_CLOSEST_HIT_SHADER |
SKIP_PROCEDURAL_PRIMITIVES` and `InstanceInclusionMask` from the config.
Non-opaque (alpha-tested) triangles depend on `FoliageMode`. `opaque` adds `RAY_FLAG_FORCE_OPAQUE`.
`ignore` adds `RAY_FLAG_CULL_NON_OPAQUE`. `stochastic` (the default) runs any-hit / candidate logic that
accepts a hit when a hash of `(InstanceIndex, PrimitiveIndex, floor(barycentrics·8))` falls below
`FoliageOpacity`. That gives a stable, leaf-like coverage pattern, since the game's alpha textures are
not accessible. A ray that escapes the TLAS (beyond the game's RT range) counts as a miss.

The hit group record table is created with **StrideInBytes = 0**, so every game instance (whatever its
`InstanceContributionToHitGroupIndex`) resolves to RTSky's single hit group. The miss table has 1
record. `MaxTraceRecursionDepth = 1`, payload 4 bytes, attributes 8 bytes.

### 7.5 Denoising
Temporal: bilinear reprojection. A tap is valid if `|z_prev − z_expected| < DepthReject·z_expected`
and `dot(n_prev, n) > NormalReject`. `len = min(len+1, MaxHistory)`, `α = max(1/len, 1/MaxHistory)`.
S, U and the moments of `lum(S)` are blended. Variance is `max(0, m2 − m1²)`; when `len < 4` it is
boosted to 4·m1². On a camera cut (> 10 m or > 30° since the previous frame) or on a reset the
history is discarded.
Spatial: `ATrousCS`, B3 5×5, step `2^i`. Weights are `w_z = exp(−|dot(N_p, P_q − P_p)| / (σ_z·z_p·step))`,
`w_n = max(0, dot(N_p, N_q))^σ_n`, and `w_l = exp(−|l_p − l_q| / (σ_l·sqrt(g3x3(var_p)) + ε))`.
Variance is filtered with squared weights. S and U share the weights.

### 7.6 Composite (relighting ratio)
For a pixel with normal N, filtered S_f, U_f and sun visibility V:
```
R      = U_f > ε ? S_f / U_f : 1                    (per channel, clamped to [0,1])
E_sky  = max(0, SH(N))                              unoccluded sky irradiance (upper hemisphere)
E_dir  = E_light · max(0, N·L) · V · DirectScale
E_gnd  = GroundAlbedo · E_h · (1 − N.z)/2           ground bounce (left unchanged)
E_art  = ArtificialAmbient · E_ref                  non-sky ambient floor (E_ref = 1 solar unit)
E_keep = E_dir + E_gnd + E_art
ratio  = (E_keep + E_sky·R) / (E_keep + E_sky·GameSkyOcclusion)
ratio  = clamp(ratio, MinRatio, MaxRatio)
out    = color · lerp(1, ratio, Strength · fade)
```
The albedo cancels: `color ≈ albedo·(E_keep + E_sky·A_game)/π`, so the replacement only needs RTSky's own
irradiance estimates. `fade` covers interiors (`InteriorStrength`), the first 0.5 m (the first-person
weapon) and pixels brighter than `EmissiveThreshold`, which are treated as self-lit.

Debug views (`DebugView=`) write straight into the HDR target: 1 R, 2 V_sun, 3 normals, 4 linear depth,
5 S_f, 6 TLAS primary-ray agreement (green = TLAS hit within 3% of depth, red = mismatch, blue =
miss), 7 final ratio, 8 history length.

### 7.7 Sun and moon
`SunModel` maps the clock (`GET_CLOCK_HOURS/MINUTES/SECONDS`) to world directions. See §9.

### 7.8 Calibration probe
`ProbeCS` traces primary rays for 64 pixels on an 8×8 grid under 8 hypotheses
(camera latency 0–3 × TLAS space world/camera-relative). A hypothesis scores a hit when
`|t_hit − t_depth| < 3%·t_depth`, counted with `InterlockedAdd`. The results are read back 3 frames later.
The CPU keeps an exponential moving average per hypothesis, updated only while the camera moves
(> 0.5 m or > 1° per frame, otherwise all latencies agree). It switches when the best hypothesis beats
the current one by more than 10 points for 30 updates. `CameraLatency` and `TlasSpace` in the INI can
pin both.

## 8. Game data (`src/Game`)

`ScriptHookV.dll` is loaded with `LoadLibraryW`, and its exports are resolved by their MSVC-decorated
names (they are C++ exports). See `ScriptHookV.cpp` for the table. If ScriptHookV is missing or
incompatible, RTSky logs the reason and stays disabled.

The script callback (`scriptRegister`) loops `scriptWait(0)` and each tick pushes a `CameraSample
{frameCount, qpc, pos, rotDeg, fovDeg, near, far}` into a 16-entry seqlock ring. It also updates the
clock, weather (`GET_CURR_WEATHER_STATE`), rain and snow levels, `IS_INTERIOR_SCENE`,
`IS_PAUSE_MENU_ACTIVE` and `IS_CUTSCENE_ACTIVE`.
The render side calls `GameData::Snapshot(latency)` to get the sample `latency` ticks before the newest.

## 9. Sun model

See `src/Game/SunModel.cpp`. The model is documented there with its constants and their origin.

## 10. Configuration
`RTSky.ini` next to the ASI. It is reloaded with a hotkey. Every key is documented in the shipped file.
Hotkeys (configurable): toggle (F10), next debug view (F11), reload config (Ctrl+F10), dump frame (Ctrl+F11).
