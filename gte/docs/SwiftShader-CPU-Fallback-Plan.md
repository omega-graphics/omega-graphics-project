# SwiftShader CPU Fallback Plan

## Goal

Give OmegaGTE a software graphics path that works when no usable GPU is
present — old machines, headless servers, broken/blacklisted ICDs, CI runners,
VMs without GPU passthrough, or hosts whose native API is unavailable
(missing D3D12 feature level, missing Metal device, missing Vulkan loader).
The path must look identical to the existing hardware backends from the
caller's point of view: `enumerateDevices()` returns one more `GTEDevice`,
`Init()` accepts it, and the rest of the engine — `OmegaGraphicsEngine`,
`GTEShaderLibrary`, `GECommandQueue`, `GERenderPipelineState`,
`GEComputePipelineState`, blits, fences, heaps, sampler state — works on top
of it without any backend-specific branching at the call site.

The chosen software implementation is **Google SwiftShader**
(<https://github.com/google/swiftshader>), a CPU-resident Vulkan ICD already
shipped in production by Chrome, ChromeOS, the Android emulator, Dawn/WebGPU,
and Skia. It implements Vulkan 1.3 core, talks SPIR-V, and ships a
loader-compatible `VK_ICD` JSON manifest plus a `libvk_swiftshader` library.
Reusing it means OmegaGTE's existing Vulkan backend already drives 95% of the
fallback for free — we just need to (a) build the ICD, (b) make the Vulkan
backend buildable on hosts whose primary backend is D3D12 or Metal, and
(c) wire device enumeration so a software device is reported with the right
`GTEDevice::Type` and `GTEDeviceFeatures`.

This document follows the multi-phase rule from `AGENTS.md`: research,
proposal, refinement, plan, then incremental implementation.

---

## 1. Research — How Other Stacks Solve This

| Stack | Software fallback | Mechanism | Notes |
|---|---|---|---|
| Direct3D | WARP (`D3D12CreateDevice` with `IDXGIFactory4::EnumWarpAdapter`) | Microsoft-shipped CPU rasterizer wired into the D3D12 runtime | Always present on Windows, no extra binary to ship |
| Metal | (none — falls back to CPU compute paths in MPS, no public software device) | n/a | Apple does not ship a software Metal device |
| Vulkan / Linux | Mesa **`lavapipe`** (Gallium llvmpipe wearing a Vulkan ICD) | Listed alongside HW ICDs in `/usr/share/vulkan/icd.d` | Stable, GPL-licensed, ships with Mesa |
| Vulkan / cross-plat | Google **SwiftShader** | Self-contained ICD, Apache-2.0, builds on Win/macOS/Linux/Android | Used by Chrome, Dawn, Skia, ANGLE-on-Vulkan, Android emulator |
| WebGPU (Dawn) | SwiftShader via Dawn's Vulkan backend | Same mechanism as proposed here | Validates that the SPIR-V → SwiftShader path is robust |
| ANGLE | SwiftShader as a Vulkan adapter | Same | Same |

**Why SwiftShader over the alternatives:**

- WARP is D3D12-only and Windows-only; we still need a fallback on macOS and
  Linux/Android. Building one cross-platform path is cheaper than three.
- lavapipe is Linux-only in practice (the Mesa build on Windows/macOS is not
  a maintained product) and pulls in LLVM. Useful on Linux as a *secondary*
  source of ICDs, but not as the primary fallback.
- Apple ships no public software Metal device, so the Metal backend has no
  WARP-equivalent to delegate to.
- SwiftShader builds on every platform OmegaGTE targets, has Apache-2.0
  licensing compatible with this repo, ships a stable Vulkan 1.3 surface
  with SPIR-V, and is the same fallback that Chrome/Dawn/ANGLE rely on, so
  it is well-exercised against the kind of shader code OmegaSL emits.

**What SwiftShader does *not* give us:**

- No ray-tracing extensions (`VK_KHR_acceleration_structure`,
  `VK_KHR_ray_tracing_pipeline`) — `GTEDEVICE_FEATURE_RAYTRACING` will be
  reported as unsupported on the SwiftShader device.
- No mesh-shader extensions — `GTEDEVICE_FEATURE_MESH_SHADER` reported off.
- No tessellation/geometry shaders by default.
- No timestamp queries (`timestampPeriod = 0`).
- Reduced texture-compression coverage (BC partial; ETC2/ASTC absent).
- Performance is far below any GPU; this is a correctness fallback, not a
  perf path.

These are not blockers — `GTEDeviceFeatures` already encodes capability bits
exactly so callers can skip features that aren't there. The fallback's job is
to keep apps running, not to be fast.

---

## 2. Proposed Solution

### 2.1 Treat SwiftShader as a third Vulkan ICD, not a fourth backend

The cheapest, most maintainable design is **not** a parallel
`GESwiftShaderEngine` next to `GED3D12Engine`/`GEMetalEngine`/`GEVulkanEngine`.
That would duplicate the entire `GEVulkanEngine` body. Instead:

- Build SwiftShader as a Vulkan ICD (its native build artifact).
- Build the existing `GEVulkanEngine` on every host — even Windows and macOS,
  where today only D3D12 / Metal compile. On those hosts the Vulkan backend
  exists *only* to drive the SwiftShader ICD; it is not the primary path.
- The Vulkan loader does the rest. SwiftShader appears as one more entry
  from `vkEnumeratePhysicalDevices`, and `enumerateDevices()` already wraps
  whatever it finds in a `GTEVulkanDevice`.

This means SwiftShader integration is, structurally, "compile the Vulkan
backend on D3D12/Metal hosts and bundle one more `.dylib`/`.dll`/`.so`."

### 2.2 Device classification — extend `GTEDevice::Type`

`GTEDevice::Type` currently has `Integrated` and `Discrete`. Add:

```cpp
using Type = enum : int {
    Integrated,
    Discrete,
    Software,   // CPU-resident rasterizer (SwiftShader, lavapipe, WARP)
};
```

When the Vulkan device-enumeration code sees
`VK_PHYSICAL_DEVICE_TYPE_CPU`, it maps to `Software`. (SwiftShader reports
`VK_PHYSICAL_DEVICE_TYPE_CPU`; lavapipe does too — this branch handles both
without naming SwiftShader specifically.)

### 2.3 Device-selection policy — `InitWithDefaultDevice()`

Today `InitWithDefaultDevice()` picks `devices.front()`. Replace with a
preference order so a `Software` device is picked only when no hardware
device is available:

1. `Discrete`
2. `Integrated`
3. `Software`

Add a public `OmegaGTE::DevicePreference` enum so callers can override the
policy (`PreferHardware`, `PreferSoftware`, `RequireHardware`,
`RequireSoftware`) — useful for tests and for apps that want deterministic
software rasterization.

### 2.4 Backend matrix after the change

| Host OS | Primary backend | Fallback backend |
|---|---|---|
| Windows | D3D12 | Vulkan + SwiftShader ICD |
| macOS / iOS | Metal | Vulkan (via MoltenVK) + SwiftShader ICD |
| Linux / Android | Vulkan (HW) | Vulkan + SwiftShader ICD (and/or lavapipe) |

On macOS the Vulkan loader needs MoltenVK only for hardware Vulkan; for
SwiftShader the loader talks to SwiftShader directly via its ICD JSON. We
can ship SwiftShader without shipping MoltenVK if hardware Vulkan on macOS
is not a goal — to be decided in §6.

### 2.5 Feature flag — opt-in build, opt-out runtime

Add a CMake option `OMEGAGTE_ENABLE_SWIFTSHADER` (default `ON`). When `OFF`,
nothing about SwiftShader is built or shipped, the Vulkan backend on
D3D12/Metal hosts is not built, and the fallback path simply doesn't exist
— useful for size-constrained builds. When `ON`, the fallback is *always
present*; selection at runtime is governed by §2.3.

Add a runtime escape hatch: env var `OMEGAGTE_DISABLE_SOFTWARE=1` causes
`enumerateDevices()` to drop `Software` devices. This is a
diagnose-the-problem switch for users who want to see "no GPU" rather than
silently fall back to CPU.

---

## 3. Refinement — Component-by-Component

### 3.1 `AUTOMDEPS` — fetch SwiftShader source

Add to `gte/AUTOMDEPS`:

```json
{
    "name": "swiftshader",
    "type": "git",
    "url": "https://github.com/google/swiftshader.git",
    "dest": "$(external_lib_path)/swiftshader",
    "platforms": ["windows", "macos", "linux", "android"]
}
```

`type: git` matches the existing pattern (see `vulkan-memory-allocator`,
`DirectXMath`, etc.). The `platforms` list spans all four hosts because the
fallback is needed on all of them; the existing `vulkan-memory-allocator`
entry restricts to `linux/android` only because VMA is consumed only by the
Vulkan backend, but after this plan the Vulkan backend builds everywhere,
so VMA's `platforms` list will need to widen to match (see §3.4).

SwiftShader has its own submodule deps (Marl, SPIRV-Headers, SPIRV-Tools,
LLVM subset) which it pulls in via its own CMake — we do **not** mirror
those into AUTOMDEPS individually. The SwiftShader CMake handles them.

If a `version_source` block (see `wtk/AUTOMDEPS:icu`) is wanted later for
auto-tracking SwiftShader releases, that can be added in a follow-up; for v1
pin to a known-good commit via a `ref` field.

### 3.2 Building SwiftShader

SwiftShader is a CMake project. The output we care about is the Vulkan ICD:

- `libvk_swiftshader.so` (Linux/Android)
- `libvk_swiftshader.dylib` (macOS)
- `vk_swiftshader.dll` (Windows)

…paired with a JSON ICD manifest (`vk_swiftshader_icd.json`) that the Vulkan
loader uses to discover it.

`gte/CMakeLists.txt` will gain:

```cmake
if(OMEGAGTE_ENABLE_SWIFTSHADER)
    set(SWIFTSHADER_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(SWIFTSHADER_BUILD_SAMPLES  OFF CACHE BOOL "" FORCE)
    set(SWIFTSHADER_BUILD_VULKAN   ON  CACHE BOOL "" FORCE)
    set(SWIFTSHADER_BUILD_PVR      OFF CACHE BOOL "" FORCE)
    set(SWIFTSHADER_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/deps/swiftshader EXCLUDE_FROM_ALL)
endif()
```

The `vk_swiftshader` target it produces is then a normal CMake target we
can copy to `${CMAKE_BINARY_DIR}/bin/` and install.

### 3.3 ICD discovery at runtime

The Vulkan loader picks ICDs from:

- `VK_ICD_FILENAMES` / `VK_DRIVER_FILES` env vars (explicit override)
- `/usr/share/vulkan/icd.d/*.json` and friends (system search paths)

Two strategies, in priority order:

1. **In-process bootstrap (preferred).** Before calling
   `vkCreateInstance`, `initVulkan()` sets `VK_ADD_DRIVER_FILES` (additive,
   loader ≥ 1.3.207) to the absolute path of the bundled
   `vk_swiftshader_icd.json`. This *adds* SwiftShader to whatever ICDs the
   loader finds without hiding the system's hardware ICD. On Linux/Android
   this is exactly the behaviour we want. On Windows/macOS, where there is
   typically no system ICD, SwiftShader becomes the sole ICD.

2. **Direct `vk_icdGetInstanceProcAddr` linkage (fallback).** On hosts where
   the Vulkan loader is not installed system-wide (typical for
   end-user Windows machines), we link `libvk_swiftshader` directly and
   bypass the loader. SwiftShader exposes `vk_icdGetInstanceProcAddr` /
   `vk_icdNegotiateLoaderICDInterfaceVersion`; we can build a tiny
   in-process loader that resolves Vulkan entry points via `dlsym` /
   `GetProcAddress` against `libvk_swiftshader` directly. This is the path
   Dawn uses for its hermetic SwiftShader builds.

Pick (2) for the v1 implementation — it's hermetic, doesn't depend on the
user having a Vulkan loader installed, and works identically on every host.
Strategy (1) becomes the fast path on Linux where the loader is reliably
present and we want SwiftShader to coexist with hardware ICDs.

### 3.4 CMake / build-system changes

`gte/CMakeLists.txt`:

- New option `OMEGAGTE_ENABLE_SWIFTSHADER` (default `ON`).
- On Windows and macOS, when the option is `ON`, also compile
  `src/vulkan/*.cpp` (currently gated by the OS branches). Define
  `TARGET_VULKAN` *in addition to* `TARGET_DIRECTX` / `TARGET_METAL` for the
  fallback build only — the `GE.cpp` `OmegaGraphicsEngine::Create` switch
  must learn to dispatch on the runtime device kind, not the build-time
  define (see §3.5).
- VMA, glm, Vulkan headers must now be available on Windows and macOS hosts
  too. Update `gte/AUTOMDEPS` to widen the `platforms` list on
  `vulkan-memory-allocator`, `glm`, and `vulkan-sdk` from
  `["linux","android"]` to all four when `OMEGAGTE_ENABLE_SWIFTSHADER` is
  `ON`. (AUTOMDEPS resolution is build-time, not platform-time — easiest is
  to just remove the platform restriction on those three; they're harmless
  on the hosts that don't compile the Vulkan backend.)
- Bundle `vk_swiftshader_icd.json` into the build output and the install
  tree alongside the library.

### 3.5 Source-level changes — the dispatch fix

Today `GE.cpp` selects the engine via `#ifdef`:

```cpp
#ifdef TARGET_DIRECTX
    return GED3D12Engine::Create(device);
#elif defined(TARGET_METAL)
    return GEMetalEngine::Create(device);
#elif defined(TARGET_VULKAN)
    return GEVulkanEngine::Create(device);
#endif
```

After this change, on Windows and macOS *both* `TARGET_DIRECTX|TARGET_METAL`
*and* `TARGET_VULKAN` may be defined. The dispatch must check the
`GTEDevice` subtype:

```cpp
if(auto v = dynamic_cast<GTEVulkanDevice*>(device.get())) {
    return GEVulkanEngine::Create(device);
}
#ifdef TARGET_DIRECTX
if(auto d = dynamic_cast<GTED3D12Device*>(device.get())) {
    return GED3D12Engine::Create(device);
}
#endif
#ifdef TARGET_METAL
if(auto m = dynamic_cast<GTEMetalDevice*>(device.get())) {
    return GEMetalEngine::Create(device);
}
#endif
```

Same shape applies to `OmegaSLCompiler::Create(device)`, which already takes
the device as a parameter.

`enumerateDevices()` becomes a thin wrapper that *concatenates* the
per-backend lists rather than picking one at compile time:

```cpp
OmegaCommon::Vector<SharedHandle<GTEDevice>> enumerateDevices(){
    OmegaCommon::Vector<SharedHandle<GTEDevice>> all;
#ifdef TARGET_DIRECTX
    appendD3D12Devices(all);
#endif
#ifdef TARGET_METAL
    appendMetalDevices(all);
#endif
#ifdef TARGET_VULKAN
    appendVulkanDevices(all);
#endif
    sortByPreference(all);   // Discrete > Integrated > Software
    return all;
}
```

The current per-backend `enumerateDevices` definitions get renamed to
`appendXxxDevices` to avoid ODR conflicts.

### 3.6 Feature reporting on the SwiftShader device

`queryVulkanFeatures` already maps `VkPhysicalDeviceFeatures` to
`GTEDeviceFeatures`. SwiftShader will simply report its real feature set
through the standard Vulkan queries, so most of this works for free. Verify
specifically:

- `VK_PHYSICAL_DEVICE_TYPE_CPU` → `GTEDevice::Software` (new branch in the
  enumeration loop).
- `props.deviceName` will be `"SwiftShader Device (LLVM N.N)"` — surface
  this verbatim so users can tell at a glance which device they got.
- `timestampPeriod` will be `0` for SwiftShader; `hasFeature(...)` will
  correctly return false for raytracing/mesh/etc.

### 3.7 OmegaSL — shader compilation

The `OmegaSLCompiler` already targets SPIR-V for the Vulkan backend (via
`shaderc`/`glslc` per `gte/CMakeLists.txt:101-115`). SwiftShader consumes
SPIR-V. Therefore: **no compiler changes are required**; the existing
SPIR-V output path drives SwiftShader unchanged.

The one thing to verify: SwiftShader's SPIR-V acceptance set. If OmegaSL
emits an extension SwiftShader rejects (e.g. ray-tracing intrinsics),
`makeRenderPipelineState` will fail at pipeline-create time on the software
device. Since we already gate those features behind `GTEDeviceFeatures`
flags, well-behaved callers will not emit unsupported shaders on a
`Software` device. We add a single sentence to
`OmegaSL-Reference.md` documenting which feature flags are off on the
software fallback.

### 3.8 Surface / presentation

This is the one genuinely new corner. SwiftShader supports
`VK_KHR_surface` + the platform surface extensions (`VK_KHR_xlib_surface`,
`VK_KHR_wayland_surface`, `VK_KHR_win32_surface`, and on macOS `VK_MVK_macos_surface`/`VK_EXT_metal_surface`). It also supports the
loader's `VK_KHR_headless_surface` extension for off-screen rendering
without a window system.

For OmegaGTE's first integration we will:

1. Use the platform-native surface extension that already matches the host
   (`VK_KHR_win32_surface` on Windows, `VK_EXT_metal_surface` on macOS,
   existing X11/Wayland on Linux). `GENativeRenderTarget` already wraps
   surfaces; the `WTK` integration layer presents into them.
2. Verify swapchain present works through SwiftShader's surface
   implementation. SwiftShader on Windows uses a software present via
   `BitBlt`; on macOS via `CALayer` upload; on Linux via shared-memory or
   X11 PutImage. Performance is poor but correctness is what we need.

If presentation through SwiftShader's native surface proves brittle on
Windows/macOS, the fallback fallback is to make the software device
`headless-only`: it can render to off-screen `GETextureRenderTarget`s that
WTK then `glReadPixels`-style copies to a DIB / `CGImage` and presents
through the platform's native window-server path. This keeps the
SwiftShader integration purely off-screen and pushes presentation back to
each native windowing layer. We pick between these in Phase 4.

### 3.9 Tests

Add a CI configuration that builds with `OMEGAGTE_ENABLE_SWIFTSHADER=ON`
and runs the existing `gte/tests` suite *with the software device forced*
(via `OMEGAGTE_REQUIRE_SOFTWARE=1`). This catches:

- Backend dispatch regressions (does `GE.cpp` route to Vulkan on a non-
  Linux host?).
- SPIR-V compatibility regressions in OmegaSL.
- Missing `GTEDeviceFeatures` gates in tests (a test that assumes ray
  tracing exists will now fail loudly on the software path).

Existing render-pass / blit / compute tests already exercise enough of the
surface to give meaningful coverage; no new fallback-specific tests are
required for v1.

### 3.10 Documentation

- Update `gte/docs/About.rst` with a "Software Fallback" section
  describing when SwiftShader kicks in, how to disable it, and what
  features are unavailable.
- Update `gte/docs/API.rst` for the new `GTEDevice::Software` enum value
  and `DevicePreference` API.
- Add a one-line note to `gte/docs/GTEDeviceFeatures-Extension-Plan.md`
  cross-referencing this doc.

---

## 4. Multi-Phase Plan

### Phase 0 — Decision lock-in (no code)

- Confirm the structural choice in §2.1 (reuse Vulkan backend) vs the
  alternative parallel-engine design.
- Pick the SwiftShader pin (commit SHA) for v1. Default: latest tag at the
  time Phase 1 lands.
- Decide §3.8: surface-through-SwiftShader vs. headless-only-with-WTK-blit.
  Default: surface-through-SwiftShader on Linux (already works); headless
  on Windows/macOS for v1, upgrade in a follow-up.

### Phase 1 — AUTOMDEPS + build-only integration

Goal: SwiftShader builds, lands `vk_swiftshader` in `bin/`, but no runtime
code touches it yet.

- Add the `swiftshader` entry to `gte/AUTOMDEPS` (§3.1).
- Widen `platforms` on `vulkan-memory-allocator`, `glm`, `vulkan-sdk`.
- Add `OMEGAGTE_ENABLE_SWIFTSHADER` option to `gte/CMakeLists.txt` (§3.4).
- Add the `add_subdirectory(deps/swiftshader)` block under that option.
- Verify `vk_swiftshader` builds on Linux, Windows, and macOS in CI (or
  locally — note the host compiler requirements: SwiftShader needs a
  recent clang/MSVC).
- No engine source changes yet. Existing tests still pass unchanged.

### Phase 2 — Vulkan backend everywhere

Goal: the Vulkan backend compiles on D3D12 and Metal hosts. Hardware
backends still take precedence; SwiftShader is not yet plumbed.

- In `gte/CMakeLists.txt`, build `src/vulkan/*.cpp` whenever
  `OMEGAGTE_ENABLE_SWIFTSHADER` is `ON`, in addition to the existing
  Linux-only path.
- Add `TARGET_VULKAN` to the public defs on Windows/macOS in this mode.
- Convert `GE.cpp::OmegaGraphicsEngine::Create` from compile-time `#ifdef`
  dispatch to runtime `dynamic_cast` dispatch (§3.5). Keep the per-backend
  `appendXxxDevices` rename minimal — just enough so multiple backends can
  coexist.
- On hosts where `TARGET_VULKAN` is added as a fallback, the Vulkan
  backend's `enumerateDevices` should *not* emit hardware physical devices
  on Windows/macOS — it should only retain the SwiftShader CPU device.
  (Filtering: skip non-`VK_PHYSICAL_DEVICE_TYPE_CPU` devices when the
  primary backend is D3D12 or Metal. We don't want to silently steal
  control from the native hardware backend.)
- Existing Linux Vulkan path is unchanged.

### Phase 3 — Software ICD wiring

Goal: SwiftShader is actually used at runtime when no hardware device is
present.

- Add the `Software` value to `GTEDevice::Type` (§2.2). Update every
  switch on `GTEDevice::Type` (search for `Discrete` / `Integrated`).
- Map `VK_PHYSICAL_DEVICE_TYPE_CPU` → `Software` in the Vulkan
  `enumerateDevices` loop.
- Implement the in-process ICD bootstrap in `initVulkan()` (§3.3) — set
  `VK_ADD_DRIVER_FILES` to the bundled `vk_swiftshader_icd.json` path,
  resolved relative to the running module.
- Implement the §3.3 strategy (2) hermetic loader path on Windows/macOS.
- Bundle `vk_swiftshader_icd.json` into the build output and install tree.
- Implement the device-preference policy (§2.3) in
  `InitWithDefaultDevice`. Add the `DevicePreference` public API.
- Honor `OMEGAGTE_DISABLE_SOFTWARE` in `enumerateDevices`.

### Phase 4 — Presentation correctness

Goal: the SwiftShader path can drive a window via WTK end-to-end on every
host.

- Stand up a minimal WTK sample app that forces `RequireSoftware` and
  verify presentation on Linux, Windows, macOS.
- If §3.8 strategy A (native surfaces) is robust, ship that. If not, fall
  back to strategy B (headless + WTK blit) for the affected platform(s),
  documented as a known limitation.
- Add `GENativeRenderTarget`-level handling for the
  `VK_KHR_headless_surface` case if strategy B is needed.

### Phase 5 — CI and documentation

Goal: regressions are caught and users know the fallback exists.

- Add a CI matrix entry: `OMEGAGTE_ENABLE_SWIFTSHADER=ON` +
  `OMEGAGTE_REQUIRE_SOFTWARE=1` running the full `gte/tests` suite.
- Update `gte/docs/About.rst`, `gte/docs/API.rst`,
  `gte/docs/GTEDeviceFeatures-Extension-Plan.md`,
  `gte/docs/OmegaSL-Reference.md` per §3.10.
- Add a runbook entry: "How to verify your build is on the software
  fallback."

### Phase 6 — Stretch follow-ups (not in v1)

- AUTOMDEPS `version_source` block to auto-track SwiftShader releases.
- lavapipe as a Linux secondary fallback for users without SwiftShader.
- A `GTEDeviceFeatures::softwareRasterizer` boolean (cheap; useful for
  callers that want to tune content quality based on the runtime path).
- Headless render-server mode that uses the software device explicitly
  (useful for thumbnail generation, server-side rendering, CI screenshot
  diffing).

---

## 5. Risks and Open Questions

- **Binary size.** SwiftShader is a sizable dependency (tens of MB). For
  size-constrained shipping configurations the `OFF` switch on
  `OMEGAGTE_ENABLE_SWIFTSHADER` must be respected end-to-end.
- **Build-time cost.** SwiftShader bundles part of LLVM and is slow to
  compile cleanly. Mitigation: cache the built artifact in CI; consider a
  prebuilt-binary AUTOMDEPS variant later (`type: archive` + per-platform
  download URL).
- **macOS Vulkan loader presence.** End-user macOS machines do not ship a
  Vulkan loader. Strategy (2) in §3.3 handles this — verify it actually
  works without the LunarG SDK installed on the user's machine.
- **Dual-define conflicts.** Defining both `TARGET_DIRECTX` and
  `TARGET_VULKAN` (or `TARGET_METAL` and `TARGET_VULKAN`) may surface
  latent assumptions in headers or sources that today expect exactly one
  target. Phase 2 needs a careful sweep — search for `#elif
  defined(TARGET_*)` chains and rewrite as runtime dispatch where they
  guard backend-selection logic, not platform-selection logic.
- **License surface.** SwiftShader is Apache-2.0; its bundled deps include
  Marl (Apache-2.0), SPIRV-Tools/Headers (Apache-2.0/MIT), a subset of
  LLVM (Apache-2.0 with LLVM exception). All compatible with this repo;
  worth a one-time audit at integration time.
- **Determinism on the software path.** SwiftShader is bit-deterministic
  for the same input; this is in fact a benefit for golden-image testing.
  Worth documenting so callers know they can rely on it for diff tests.

---

## 6. Decisions Needed Before Phase 1

(These are the items I do not have enough context to settle without your
judgment of the system.)

1. **Pin policy.** Track SwiftShader by tag, by release branch, or by a
   hand-picked commit? Other AUTOMDEPS entries are split — `libjpeg-turbo`
   pins `2.0.x`, most others float. My recommendation is a hand-picked
   commit for v1 and a `version_source` block in a follow-up.
2. **macOS Vulkan posture.** Do we want to ship MoltenVK alongside
   SwiftShader so macOS users can also get hardware Vulkan? Or is Metal
   the only macOS hardware path forever, with SwiftShader as the *only*
   reason the Vulkan backend exists on macOS? My recommendation is the
   second — keep the macOS surface area small, Metal stays primary,
   Vulkan is software-only on macOS. 

   ALEX: Metal should be the primary. Vulkan is only for software-based rendering.

3. **Shipping form.** Is SwiftShader linked in (one binary, fewer files
   to ship) or kept as a sidecar `.dll`/`.dylib`/`.so` with a JSON
   manifest (matches the Vulkan ecosystem convention, easier to swap)?
   My recommendation is sidecar — it matches the loader's expectations
   and lets users replace the ICD without rebuilding OmegaGTE.

   ALEX: Sidecar
4. **Phase 4 strategy default.** Native surfaces vs headless-blit on
   Windows/macOS for v1. My recommendation is headless-blit — gets us a
   working fallback faster, native-surface support can land later
   without breaking callers.

A short conversation on these four items resolves everything Phase 1 needs.
