# RTSky architecture

RTSky adds **ray-traced sky lighting** to GTA V Enhanced (the DirectX 12 PC edition). For every
on-screen surface it traces world-space rays through the game's own DXR scene (the top-level
acceleration structure (TLAS) that Rockstar builds each frame for RTGI and reflections). Rays that
escape to the sky pick up physically based sky radiance. Rays that hit geometry are occluded. The result
is soft, directional sky shadowing that the game's own ambient term only fakes. RTSky
composites it into the game's HDR lighting buffer **inside the game's frame**, before fog,
transparents, post-processing, upscaling and UI.

RTSky is native code: one `RTSky.asi`, loaded by an ASI loader. It hooks D3D12 directly and never
submits command lists of its own. Every command it records goes into the game's command lists, between two
of the game's own commands, and all state it touches is restored.

## 1. Frame data flow

```
 game                                             RTSky
 ────                                             ─────
 D3D12CreateDevice (MinHook detour)          ──►  patch device / command list / queue vtables
 Create{RTV,DSV,SRV}, CopyDescriptors*       ──►  DescriptorTracker: CPU handle -> resource, format, flags
 recording threads:
   BuildRaytracingAccelerationStructure      ──►  TlasTracker: clone the scene TLAS into RTSky's ring
   OMSetRenderTargets / Begin/EndRenderPass  ──►  CommandListTracker: binding log, draws, viewport,
   ResourceBarrier / Barrier                 ──►    observed states, full root + pipeline state
   Set*Root*, SetPipelineState(1), heaps     ──►
   binding closes (next OMSetRenderTargets,  ──►  Renderer::OnBindingClosed
     EndRenderPass, Close)                          G-buffer pass ends  -> PREPARE  (depth -> linear depth, normals)
                                                    HDR lighting ends   -> COMPOSITE (sky LUTs, DXR trace,
                                                                           denoise, relight in place)
 ExecuteCommandLists                         ──►  FrameAnalyzer: binding logs in GPU order -> rules
                                                  GpuLifetime: fence signal on that queue for lists
                                                  that carry RTSky work, release completed objects
 ScriptHookV script fiber (game thread)      ──►  GameData: camera ring, clock, weather, flags
```

Present is deliberately not hooked: with DLSS/FSR frame generation it runs on the presenter's thread,
several times per game frame, and it is not a usable frame clock.

## 2. Source layout

```
CMakeLists.txt                  builds RTSky.asi; compiles shaders with DXC (-Fh) and embeds them
cmake/toolchain-mingw64.cmake   MinGW cross build
config/RTSky.ini                default, fully commented configuration
shaders/RTSkyShared.h           C++/HLSL shared layouts, register map, flags
shaders/*.hlsl(i)               §6
src/Main.cpp                    DllMain, ScriptHookV script loop, hotkeys
src/Common/                     Log, Config (INI), Math, D3D12Compat (MSVC/MinGW struct returns, IIDs)
src/Hooks/                      VTableHook (per-vtable originals), VTableIndices.c (offsetof-derived slots),
                                D3D12Hooks (hook bodies, installation), Bypass
src/Track/                      CommandListTracker, DescriptorTracker, TlasTracker, FrameAnalyzer
src/Render/                     Renderer, RtPipeline, Barriers, GpuLifetime, Calibration, ShaderBlobs
src/Game/                       ScriptHookV (decorated exports), GameData, SunModel, Weather
third_party/minhook             MinHook (BSD-2-Clause)
tests/MathTests.cpp             host tests: camera basis, sun orbit, weather
```

## 3. Hooking (`src/Hooks`)

### 3.1 Acquisition
* **Early path (normal).** ASI loaders for GTA V Enhanced load plugins during CRT start-up, before the
  game touches D3D12. `DllMain` initialises MinHook and detours `d3d12.dll!D3D12CreateDevice`. If
  d3d12.dll is not loaded yet, it does this from an `LdrRegisterDllNotification` callback when it loads.
  When the game creates its device (Streamline's interposer forwards to the real export), the detour
  patches the device vtable. It then creates a DIRECT queue and DIRECT/COMPUTE command lists on that
  device and patches their vtables. Every render target the game creates afterwards is tracked.
* **Late path (fallback).** If the device existed before RTSky loaded, the script thread asks
  D3D12 for a device on the high-performance adapter after 5 s. D3D12 devices are singletons per
  adapter, so this returns the game's device, and the same patching follows. Render targets created
  before that point are unknown until the game recreates them; the log says so.
* No dummy device is ever created before the game's own: that would become the singleton and
  break Streamline's "initialise before any DX call" rule.

### 3.2 Vtables
Slot indices are not hand-counted. `VTableIndices.c` is compiled as C and derives
`offsetof(<Iface>Vtbl, Method) / sizeof(void*)` from the SDK's C interface definitions, with
`_Static_assert`s on known values. `VTableHook` keeps one table of originals per distinct vtable:
* the ExecuteCommandLists hook patches unseen command-list vtables lazily (debug layer, runtime-bypass
  vtables), but only DIRECT/COMPUTE lists that implement `ID3D12GraphicsCommandList7` on the same
  pointer, because RTSky writes slots up to `Barrier`. Other classes are remembered and left alone;
* a hook entered through a vtable RTSky never patched (a copy of a patched one) adopts it on the
  spot. Runtime/driver entries become its originals, anything else falls back to the first table,
  so a hook never calls null;
* `VerifyHooks()` (every 2 s) re-applies slots that were overwritten, adopting the new pointer as the
  original only if it lies in the D3D12 runtime or a GPU driver. A third-party hook on top of RTSky
  is left chained;
* slot writes share one process-wide lock and only make the page `PAGE_READWRITE`;
* if installation fails halfway (old runtime), everything is unpatched again.

The module pins itself in `DllMain`, and the singletons are never destroyed: hooks, the loader
notification and GPU-lifetime deleters may still run at process exit.

Hooked: device `CreateCommandList(1)`, `Create{ShaderResource,RenderTarget,DepthStencil}View`,
`CopyDescriptors(Simple)`, `CreateCommandSignature` (an indirect execution counts as a draw only when
its signature draws). Command list: `Close`, `Reset`, `Draw(Indexed)Instanced`, `ExecuteIndirect`, `ExecuteBundle`,
`RSSetViewports`, `SetPipelineState(1)`, `ResourceBarrier`, `Barrier`, `SetDescriptorHeaps`, all root
signature and root argument setters, `OMSetRenderTargets`, `ClearDepthStencilView`,
`Begin/EndRenderPass`, `BuildRaytracingAccelerationStructure`, `DispatchRays`. Queue: `ExecuteCommandLists`.

### 3.3 Bypass
`HookBypass` is a thread-local counter set around everything RTSky records or creates. Every hook
forwards straight to the original while it is active, so RTSky's own calls are never tracked as game state.

## 4. Tracking (`src/Track`)

* **DescriptorTracker**: sharded map from CPU descriptor handle to resource, formats, size, flags and
  DSV read-only flags (RTV, DSV and acceleration-structure SRVs only). RTV/DSV copies follow `CopyDescriptors*`.
* **CommandListTracker**: one `ListState` per list. Recording a list is single-threaded, so it needs
  no lock, and a thread-local cache makes lookups cheap. A private-data "destruction watch" drops
  everything a destroyed list kept alive. RTSky never injects into a list whose Reset it did not
  see, or after the list executed a bundle (inherited root state unknown). It holds:
  - root state: heaps, both root signatures, every root argument (tables, CBV/SRV/UAV, 32-bit
    constants) and the last PSO or state object. Root-signature changes and heap changes drop exactly
    what D3D12 invalidates. `RestoreState` replays all of it.
  - binding log: render targets and depth (resolved through the descriptor tracker), viewport, draw
    count, read-only depth, depth clear value, the barrier sequence at the last draw, and per-list
    ordinals of MRT bindings (3 or more RTVs plus depth) and float-HDR bindings.
  - observed barriers per resource: the state of subresource 0 (legacy state, or enhanced layout with
    its `SyncAfter`/`AccessAfter`) as left by the last barrier covering it, whether a split barrier
    (`BEGIN_ONLY` / `SYNC_SPLIT`) is still open, plus a global record of which barrier API the game
    uses per resource;
  - render passes that suspend or end with `PRESERVE_LOCAL_*`: nothing may be recorded after them, so
    they are never injection points.
* **TlasTracker**: every top-level build is inspected. The scene TLAS is the largest one by instance
  count, or the `TlasSelect`-th. It is cloned on the same command list right after its build
  (`UAV barrier; CopyRaytracingAccelerationStructure(CLONE); UAV barrier`) into a ring of 8 RTSky
  buffers sized by `GetRaytracingAccelerationStructurePrebuildInfo`. RTSky binds the clone, so it never
  depends on how the game buffers or rebuilds its own TLAS. Recording order says nothing about GPU
  order, so:
  - a clone stays private to its list until that list is **submitted**; the ExecuteCommandLists hook
    then publishes it with the queue and the `GpuLifetime` fence value of the submission;
  - Composite prefers a clone recorded earlier in its own list (ordered by the list). Otherwise it
    binds, provisionally, the newest published clone that was produced on the queue running the
    composite (ordered by submission) or whose producing submission has already completed;
  - the TLAS is bound through a per-slot descriptor (descriptor table, `DESCRIPTORS_VOLATILE`), not
    a root SRV. When the list is submitted, the ExecuteCommandLists hook knows the GPU order and
    rewrites the descriptor with the newest clone that runs before it: one written by an earlier list
    of the same submission, or one submitted earlier on the same queue. A frame whose TLAS is built
    in a separate list submitted together with (or before) the lighting list therefore traces its
    own BVH. Only an async-compute build that has not completed yet leaves the previous frame's;
  - RTSky never makes one queue wait for another, which could deadlock with the game's own
    cross-queue waits. If the composite list moves to a different queue after its clone was chosen,
    the ExecuteCommandLists hook inserts one `Wait` on the producer's already-signalled fence and
    logs it;
  - the last 4 published clones are remembered without pinning ring slots; a slot generation
    counter tells whether a remembered clone still exists;
  - if a clone cannot be made (out of memory) the frame is not traced; the game's own TLAS memory is
    never bound, because it may be rebuilt or reallocated while the trace runs;
  - a ring slot is rewritten only when no list that wrote or reads it can still be in flight (each
    slot has a busy token, carried by every reference). A list's busy tokens leave it at its first
    submission and are released when the GPU has finished that submission, not when the game next
    resets the list: games keep closed lists for many frames, and holding the tokens until the reset
    pinned every slot (v0.1.1 then allocated a new, 25% larger buffer on every build until multi-GB
    allocations failed). When all slots are busy, a new buffer sized for that build is made.
  Builds of opacity-micromap arrays or BLASes with OMM triangles are detected (see §5.4).
* **FrameAnalyzer**: consumes binding logs at ExecuteCommandLists, so in GPU order. The G-buffer is the
  MRT signature with the most draws. Consecutive G-buffer bindings not separated by an HDR binding
  (re-binds, lists split across threads, suspended render passes) form one *phase*; a frame is a
  phase plus everything up to the next one. Prepare goes after the phase's last binding. The HDR
  lighting pass is the `CompositeCandidate`-th float-format binding of G-buffer size after the phase.
  A rule is a format/size signature plus the per-list ordinal, and it must match exactly one binding
  per frame. If the ordinal alone is ambiguous, the number of G-buffer (or HDR) bindings recorded
  earlier in the same list is added as a discriminator; if that is still ambiguous, nothing is
  armed. Rules arm after `StableFrames` identical frames and disarm when the structure changes
  (menus, loading: a phase that grows past 64 bindings without lighting). The depth clear value of
  the G-buffer depth (0 or 1) decides reversed or standard Z. Num . dumps one frame.
  `tests/AnalyzerTests.cpp` runs these scenarios against the real sources.

## 5. Renderer (`src/Render`)

### 5.1 Initialisation
On the first matching binding the renderer starts a worker thread on the game's device. It checks
for DXR tier 1.1, creates the root signature, ten compute PSOs and the DXR state object, the
descriptor heap, the LUTs, the sky-data and probe buffers, a 64-slot upload ring for constants and a
readback ring. Injections are skipped until it is ready.

### 5.2 Binding model
One root signature serves every compute pass and is the DXR global root signature: root CBV b0
(`FrameConstants` slot), a one-descriptor table t0/space1 (TLAS clone, late-bound), 8 root
constants b1 (`PassConstants`), a
12-SRV table (t0–t11) and an 8-UAV table (u0–u7), plus static linear and point samplers. The
shader-visible heap holds:
* 64 **slot tables**, one per injection in flight, for views of game resources (game depth SRV,
  HDR target UAV);
* the **global tables** for the atmosphere passes;
* 3 **set regions** for resolution-dependent tables, so a resize never rewrites descriptors the GPU
  may still be reading.

### 5.3 Prepare (end of the G-buffer pass)
The depth was just written through a writable DSV, so it is in `DEPTH_WRITE` (or layout
`DEPTH_STENCIL_WRITE`) unless a barrier recorded after the last draw says otherwise. RTSky moves only
the depth plane (subresource 0 / plane 0) to a shader-read state using the game's barrier API for that
resource, runs `PrepareCS` (linear view depth plus normals reconstructed from depth), and moves it back.
With enhanced barriers the transition waits with `SYNC_ALL` on the access scope the game's last barrier
opened (or every access the layout allows), and the restore re-opens exactly that scope. Injection is
skipped while a split barrier on the resource is open.
The camera for the frame is chosen here (latency from calibration), together with the 4 latency
hypotheses for the probe.

### 5.4 Composite (end of the HDR lighting pass)
1. Sky: transmittance and multiple-scattering LUTs, sky-view LUT, SH projection (`SkyProjectCS`), all
   every frame (the two static LUTs cost about 17K threads; a record-time "already built" flag would
   be wrong for a list that is never submitted).
2. Trace: `DispatchRays` on the DXR pipeline (or `RTSkyInlineCS` with `Path=inline`) against the TLAS
   clone. When the game uses opacity micromaps, only the pipeline path on tier 1.2 (with
   `ALLOW_OPACITY_MICROMAPS`) may trace.
3. `TemporalCS`, then `ATrousCS` × `Iterations`.
4. `ProbeCS` while calibrating; its results are copied into the slot's readback region.
5. Relighting: in place through a typed UAV when the HDR target allows UAV access, otherwise through a
   scene copy (copy out, compute, copy back). The target's state is `RENDER_TARGET` (it was just drawn
   to) unless a barrier observed after the last draw says otherwise. Targets with several mips or
   array slices are skipped: only the bound subresource's state is known.
Until the probe confirms the camera and TLAS space (`MinCalibrationScore`), everything except the
relighting runs (debug views still draw).

### 5.5 States and lifetimes
* RTSky textures rest in `NON_PIXEL_SHADER_RESOURCE`, and each pass returns them there. Buffers start
  each list in `COMMON` (they decay after every ExecuteCommandLists).
* Everything an injection references (slot, resource set, TLAS clone, scene copy) is attached to the
  game's list as a `shared_ptr`. When that list is submitted, `GpuLifetime` signals a per-queue fence
  and keeps a copy until the fence passes. A list that is reset without being executed drops its
  attachments directly. Slots, sets and buffers recycle themselves when the last reference goes. No
  thread ever waits on the GPU.
* There are two kinds of attachment. **Memory** (`Attach`: resources, the resource set, the TLAS clone
  buffer) stays with the list until its reset, because a closed list may legally be executed again.
  **Busy tokens** (`AttachBusy`: injection slots, TLAS clone ring slots) move into the fence batch of
  the list's first execution, so a slot is free once the GPU is done with it. A list that carries
  RTSky work and is executed a second time stays memory-safe, but its slots may already hold newer
  data; RTSky logs a one-time warning when that happens.
* Probe readbacks carry a frame stamp written by the GPU, so results from slots that never executed are ignored.

## 6. Shaders (`shaders/`)

| File | Entry / target | Purpose |
|---|---|---|
| `AtmosphereCS.hlsl` | `TransmittanceCS`, `MultiScatterCS`, `SkyViewCS` / cs_6_0 | Hillaire 2020 LUTs |
| `SkyProjectCS.hlsl` | cs_6_0 | SH L2 of the upper-hemisphere sky (clear + CIE overcast), sun/moon irradiance |
| `PrepareCS.hlsl` | cs_6_0 | linear depth + normals from depth |
| `RTSkyLib.hlsl` | lib_6_3: `RayGen`, `Miss`, `ClosestHit`, `AnyHit` | DXR pipeline |
| `RTSkyInlineCS.hlsl` | cs_6_5 | RayQuery variant |
| `TemporalCS.hlsl` | cs_6_0 | reprojection + accumulation |
| `ATrousCS.hlsl` | cs_6_0 | edge-avoiding à-trous filter |
| `CompositeCS.hlsl` | cs_6_0 | relighting + debug views |
| `ProbeCS.hlsl` | cs_6_5 | camera / TLAS-space calibration |

`TraceCommon.hlsli` + `TracePixel.hlsli` are shared by both trace paths. They differ only in
`TraceVisibility` / `TraceDistance`.

## 7. Algorithms

### 7.1 Conventions
GTA V world space is right-handed and Z-up (X east, Y north), in metres. The camera looks down its
local +Y. `GET_FINAL_RENDERED_CAM_ROT(2)` gives degrees (x pitch, y roll, z yaw) with
`R = Rz(yaw)·Rx(pitch)·Ry(roll)` (tested in `tests/MathTests.cpp`). The FOV is vertical.
Shaders never use matrices: `FrameConstants` carries `right·tan(fovX/2)`, `up·tan(fovY/2)`, `forward`,
so `posRel = (forward + ndc.x·right' + ndc.y·up')·viewZ`. TLAS-space position = `posRel + tlasOffset`,
where `tlasOffset` = camera position (world-space TLAS) or 0 (camera-relative TLAS).
Depth → viewZ: reversed finite `n·f/(d(f−n)+n)`, reversed infinite `n/d`, standard `n·f/(f−d(f−n))`.

### 7.2 Sky
Hillaire 2020 with the reference implementation's Earth constants (radii 6360/6460 km; Rayleigh
(5.802, 13.558, 33.1)e-3/km, scale height 8 km; Mie scattering 3.996e-3, extinction 4.44e-3/km, scale
height 1.2 km, g = 0.8; ozone (0.650, 1.881, 0.085)e-3/km as a tent from 10 to 40 km). LUTs are
computed for unit illuminance of the sky's light (the sun, or the moon more than 10° after sunset).
The weather blends the clear sky towards a CIE overcast sky `L_z(1+2cosθ)/3`. `L_z` is chosen so that
its horizontal irradiance (`L_z·7π/9`) equals `OvercastTransmission × (sun + clear sky)`. Below the
horizon the sky radiance is 0, because ground bounce is the game's own GI.

### 7.3 Trace and ratio estimator
Per pixel and per ray: a cosine-distributed direction (R2 sequence with interleaved-gradient-noise
rotation), sky radiance `L`, and visibility split at `NearFieldRadius`:
`U += L·V(0, r_near)`, `S += L·V(0, r_max)`. One soft sun/moon ray gives `V_sun`. Occluders closer than
`r_near` are left to the game's own AO, so only large-scale sky shadowing is added. S and U are denoised
with identical weights, and `R = S/U` (per channel) is the extra sky visibility. Visibility rays use
`ACCEPT_FIRST_HIT_AND_END_SEARCH | SKIP_CLOSEST_HIT_SHADER | SKIP_PROCEDURAL_PRIMITIVES`. Non-opaque
triangles get a stable procedural coverage (`FoliageOpacity`), or are forced opaque or culled.

### 7.4 Denoising
Temporal: bilinear world-space reprojection with depth and normal rejection, history length up to
`MaxHistory`, luminance moments for variance (boosted for short histories). History is reset on
resize, camera cuts (more than 10 m or 30°) and skipped frames. Spatial: SVGF-style 5×5 B3 à-trous with
plane-distance, normal-power and variance-guided luminance weights.

### 7.5 Composite
```
E_sky  = SH(N)                                    unoccluded sky irradiance
E_dir  = E_light · max(0, N·L) · V_sun · DirectScale
E_gnd  = GroundAlbedo · E_horizontal · (1 − N.z)/2
E_keep = E_dir + E_gnd + ArtificialAmbient
ratio  = clamp((E_keep + E_sky·R) / (E_keep + E_sky·GameSkyOcclusion), MinRatio, MaxRatio)
out    = color · lerp(1, ratio, Strength · fade)        fade: interior, cutscene, near, distance
```
The albedo cancels because the game's pixel ≈ albedo·(E_keep + E_sky·A)/π.

### 7.6 Calibration
`ProbeCS` traces 64 jittered primary rays per frame under 8 hypotheses (camera latency 0–3 × TLAS
space world/camera) and counts the hits within 3% of the depth-buffer distance. `Calibration` keeps an
EMA per hypothesis (latency only while the camera moves) and switches after 20 consistent votes.
`MinCalibrationScore` gates the relighting.

### 7.7 Sun and moon
`time.xml` orbit as used by CodeWalker: `dc = π(0.5 + (t − 6)/14)`,
`sun = (sin dc, −cos dc·cos 122°, −cos dc·sin 122°)`. The sun rises due east at 06:00, stands due south
at 58° at 13:00 and sets due west at 20:00. The moon follows the same orbit 12 h later; its light is
about 0.1% of the sun's and barely matters.

## 8. Game data (`src/Game`)
ScriptHookV is loaded with `LoadLibrary` and its MSVC-decorated exports are resolved by name. The script
loop samples, each tick, `GET_FRAME_COUNT`, `GET_FINAL_RENDERED_CAM_COORD/ROT/FOV/NEAR_CLIP/FAR_CLIP`
into a 16-entry seqlock ring, along with the clock, `GET_CURR_WEATHER_STATE`, rain and snow levels,
interior, pause, cutscene and loading flags.

## 9. Configuration
`RTSky.ini` sits next to the ASI and is reloaded with Num 0; every key is documented there.
Hotkeys are on the numpad (Num 1 toggles RTSky; the rest switch individual features, see the README).
The script thread draws status lines and hotkey confirmations with the game's UI text natives
(`src/Game/Overlay.cpp`).
