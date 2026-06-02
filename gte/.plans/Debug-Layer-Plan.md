# OmegaGTE Debug Layer Plan

## Status

| Backend | Slice landed | Plan section |
|---------|--------------|--------------|
| D3D12   | Yes — see `src/d3d12/GED3D12.cpp` constructor + `~GED3D12Engine` | — |
| Vulkan  | Yes — see `initVulkan()` in `src/vulkan/GEVulkan.cpp` + `src/vulkan/VulkanExtStubs.cpp` | §2 |
| Metal   | Yes (v1) — capture + env-var docs + comment fix; API-validation toggle dropped (no Metal API) | §3 |

The cross-cutting pieces (`GTEInitOptions`, `isDebugLayerEnabled()`,
`isGpuBasedValidationEnabled()`, the runtime-gated `DEBUG_STREAM` macro,
the `OMEGAGTE_DEBUG` CMake flag) shipped with the D3D12 slice and are
already consumed by every backend's `DEBUG_STREAM` calls. §2 (Vulkan)
has since landed, so turning the layer off now also gates Vulkan's
backend validation (layer + messenger + GPU-AV). Metal's backend
validation is still hard-wired on; §3 closes that remaining gap.

## Design recap

- One process-wide toggle, resolved once at `Init()` from
  `GTEInitOptions::debugLayer` against the `OMEGAGTE_DEBUG` compile
  default. Frozen for the process lifetime; no per-engine re-toggle.
- `gpuBasedValidation` is a separate bool — ignored unless the debug
  layer is on. Heavy (5–10× draw cost on D3D12 GBV / Vulkan GPU-AV).
- `DEBUG_STREAM` runtime-checks the flag and stays a no-op in release
  when off. Strings remain in the binary; the branch is a single atomic
  load that the CPU's branch predictor handles for free in a hot loop.
- Backend validation messages funnel into `DEBUG_STREAM` so a single
  output stream covers everything.

## 2. Vulkan plan

**Status: landed.** All four items below shipped in
`src/vulkan/GEVulkan.cpp::initVulkan()` and `src/vulkan/VulkanExtStubs.cpp`.
Line numbers below reflect the as-built code, not the original
pre-change wiring.

### Where the wiring lives

All in `src/vulkan/GEVulkan.cpp::initVulkan()` unless noted:

- L128–129: snapshots `isDebugLayerEnabled()` and
  `isGpuBasedValidationEnabled()` once at function entry.
- L167–171: queries `VK_EXT_debug_utils` and pushes it into
  `requiredInstanceExtensions` *only* when the debug layer is on.
- L173–190: scans for and enables `VK_LAYER_KHRONOS_validation` *only*
  when the debug layer is on; L195–211 probes the layer for
  `VK_EXT_validation_features` (only when GBV is also requested).
- L246–291: chains `VkValidationFeaturesEXT` onto
  `VkInstanceCreateInfo.pNext` (when GBV is available) and creates
  `g_debugMessenger` with WARNING+ERROR+INFO severities. The callback
  at L97–113 writes through `DEBUG_STREAM`, not `std::cerr`.
  `cleanupVulkan()` (L311–325) destroys the messenger.

### What landed

1. **Gate extension + layer selection on `isDebugLayerEnabled()`.**
   When off: do not push `VK_EXT_debug_utils` into
   `requiredInstanceExtensions`, do not push
   `VK_LAYER_KHRONOS_validation` into `enabledLayers`, do not create the
   debug messenger. Keep `hasDebugUtils` as a local query result — but
   only act on it when the flag is on.

2. **Re-route the existing `vulkanDebugCallback` (L97–113) into
   `DEBUG_STREAM`.** Drop the `std::cerr` write; instead format the same
   severity-prefixed line through `DEBUG_STREAM`. This unifies output
   with the rest of the engine — and since the messenger only exists
   when the debug layer is on, the runtime check inside `DEBUG_STREAM`
   never fires negatively here.

3. **GPU-assisted validation.** When
   `isGpuBasedValidationEnabled()` is true and the layer is on, request
   `VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT` via a
   `VkValidationFeaturesEXT` chained on `VkInstanceCreateInfo.pNext`.
   Requires the layer to advertise the feature — query
   `vkEnumerateInstanceExtensionProperties("VK_LAYER_KHRONOS_validation",
   …)` for `VK_EXT_validation_features` before requesting it; silently
   downgrade to plain validation if missing.

4. **`vkSetDebugUtilsObjectNameEXT` calls** (scattered at 2992, 3047,
   3257) remain compiled in but become no-ops when the layer is off.
   Implemented via a *centralized interposer* rather than the
   per-callsite null-checks this section originally proposed:
   `src/vulkan/VulkanExtStubs.cpp` defines `vkSetDebugUtilsObjectNameEXT`
   itself. It reads the cached `OmegaGTE::g_pfnSetDebugUtilsObjectNameEXT`
   (set at `GEVulkan.cpp:299–302`, non-null *only* when the debug layer
   is on AND `VK_EXT_debug_utils` was actually enabled) and returns
   `VK_SUCCESS` when the pointer is null. The three callsites stay clean
   and unconditional — the no-op behavior lives in exactly one place
   instead of being scattered across every backend object, and the
   pre-existing crash-on-null-proc-address bug is closed at the same
   point. (Previous draft of this item proposed null-checking at every
   callsite; superseded by the interposer.)

### Edge cases

- **Layer present but app opted out via `VK_INSTANCE_LAYERS` env.** Our
  enumeration finds the layer; the loader may still refuse to load it.
  Today this is silently broken. Don't add new handling — note it.
- **MoltenVK on macOS.** Out of scope for v1; macOS uses the Metal
  backend.

### Verification

- Build with `-DOMEGAGTE_DEBUG=OFF`, run any sample, confirm
  `VK_LAYER_KHRONOS_validation` does not appear in
  `vkEnumerateInstanceLayerProperties` output of the running process
  (via `vkconfig` or environment trace). Confirm zero `[VVL …]` lines
  on stderr.
- Build with `-DOMEGAGTE_DEBUG=ON`, deliberately trigger a validation
  error (e.g. bind a destroyed resource), confirm the error reaches
  `DEBUG_STREAM` formatted with the `[VVL ERROR]` prefix.
- Measure draw-call throughput in a tight loop with the layer on vs.
  off — expect roughly 1.5–3× speedup with the layer off, plus ~10×
  more on top if GBV was on.

## 3. Metal plan

### Constraints

There is exactly one real OS-level constraint, and one stale source
comment — both addressed as we landed this work.

**Real constraint — process-env validation.** Metal API validation is
set via the `METAL_DEVICE_WRAPPER_TYPE=1` environment variable, read by
`MTLCreateSystemDefaultDevice` *before* device construction. We don't
own that call site for users who pass us an existing `id<MTLDevice>` —
and even for `InitWithDefaultDevice`, the process env is already locked
in by the time `Init()` runs in practice (child-process spawn aside).

There is **no public runtime API** to flip Metal API validation on after
launch. A previous draft of this plan claimed
`MTLCaptureManager.setShouldEnableValidation:` could do this on macOS 11+
— that was wrong; no such property/method exists on `MTLCaptureManager`
(verified against the SDK headers and Apple docs, May 2026). Metal API
validation is therefore env-var / Xcode-scheme only. `debugLayer` gates
`DEBUG_STREAM` and programmatic capture, but **not** native API
validation. See `gte/docs/Metal-Debug.md` for the env-var path.

**Stale source comment (fixed).** The comment formerly at
`src/metal/GEMetal.mm:943–944` (a two-line prose note inside the
`GEMetalEngine` constructor — *not* a commented-out `MTLCaptureManager`
block; there was never any capture code) attributed a past
drawable-presentation regression to GPU capture intercepting Metal calls
via `CaptureMTLDevice`. That attribution was wrong — the drawable was
never the issue. The WTK compositor used to render to a texture then blit
to the drawable, and it wasn't showing because Metal forbids `MTLTexture`
creation on multiple threads. That bug is fixed and unrelated to capture.
The comment was replaced by the live, flag-gated capture path (item 3).

### Scope for v1

1. **`DEBUG_STREAM` already gates Metal logging** — done by the
   cross-cutting slice. No backend change required for that axis. ✅

2. **Programmatic API-validation toggle — dropped (not possible).**
   The Metal API has no runtime validation toggle (see Constraints), so
   this cannot mirror D3D12's `EnableDebugLayer`. Native API validation
   is driven by `METAL_DEVICE_WRAPPER_TYPE=1` / the Xcode scheme,
   documented in `gte/docs/Metal-Debug.md` (item 4). No code change.

3. **Programmatic GPU capture.** ✅ New `GTEInitOptions::captureOnInit`
   bool (+ `captureFilePath`). When set together with `debugLayer`
   resolving on, `GEMetalEngine`'s constructor starts an
   `MTLCaptureManager` capture session (via `startCaptureWithDescriptor:`
   to a `MTLCaptureDestinationGPUTraceDocument`) and `~GEMetalEngine`
   stops it at `Close()`. Path is taken from `captureFilePath`, else
   defaulted to `omegagte-<pid>-<ts>.gputrace` in the working directory.
   Gated behind its own flag, not just `debugLayer`, because traces grow
   to multi-GB quickly. **Requires the embedding app to enable capture**
   via `MetalCaptureEnabled=YES` in its Info.plist (or
   `MTL_CAPTURE_ENABLED=1`) — a library can't grant this. When it isn't
   set, `supportsDestination:` returns NO and OmegaGTE logs + skips
   (no crash). Reached via the `isCaptureOnInitEnabled()` /
   `captureOutputPath()` accessors, resolved in `resolveDebugFlags`
   alongside the other debug flags (since `OmegaGraphicsEngine::Create`
   doesn't receive `GTEInitOptions`).

4. **Document the env-var path.** ✅ New `gte/docs/Metal-Debug.md`
   explaining:
   - `METAL_DEVICE_WRAPPER_TYPE=1` — Metal API validation. Required
     when targeting macOS <11 or when the user constructed the
     `id<MTLDevice>` themselves.
   - `MTL_DEBUG_LAYER=1` — extended API validation (Xcode-style).
   - `MTL_SHADER_VALIDATION=1` — shader bounds checking (macOS 14+).
   - `METAL_ERROR_MODE=5` / `METAL_ERROR_MODE=3` — abort modes.
   - `MTL_CAPTURE_ENABLED=1` — alternative to the Info.plist capture key.
   - How to set them in an Xcode scheme vs. on the command line.
   - Note that `GTEInitOptions::debugLayer = Enabled` handles engine
     logging + capture automatically; env vars are for native API/shader
     validation, cross-version coverage, and the user-supplied-device path.

5. **Fix the stale comment** at `src/metal/GEMetal.mm:943–944`. ✅
   Replaced the two-line prose comment with the live, flag-gated capture
   path (item 3), leaving a pointer to `GTEInitOptions::captureOnInit` so
   future readers know where the toggle lives.

### 3.1. Future Metal work (not v1)

With the stale-comment misattribution corrected and capture promoted
into v1, the remaining items are independent OS-tier additions driven
by need.

- **`MTLLogState` (macOS 15+ / iOS 18+).** Per-device log delivery via
  `MTLLogStateDescriptor`. When enabled, supersedes the env-var path
  for shader logging and funnels device logs into `DEBUG_STREAM`.
- **`MTLCommandBufferDescriptor.errorOptions =
  .encoderExecutionStatus`** for per-encoder GPU error attribution.
  Cheap; can ride along with the debug-layer flag.
- **`addCompletedHandler` failure inspection** on every command buffer
  when the debug layer is on. Pull `commandBuffer.error` and route into
  `DEBUG_ERROR(QUEUE, …)` from §4 once that lands.

### Verification (v1)

- Build with `-DOMEGAGTE_DEBUG=OFF`, run macOS sample, confirm zero
  `[GEMetalEngine_Internal]` lines on stdout.
- Build with `-DOMEGAGTE_DEBUG=ON`, confirm `[GEMetalEngine_Internal]`
  lines *do* appear (engine logging gated by the debug layer).
- Confirm native Metal API validation is driven by the environment, not
  the debug layer: with `METAL_DEVICE_WRAPPER_TYPE=1` set, deliberately
  trigger an API misuse (e.g. resource use after free) and confirm Metal
  raises the expected diagnostic; without it, no diagnostic — regardless
  of `OMEGAGTE_DEBUG`. (There is no programmatic toggle; see Constraints.)
- Run a sample with `captureOnInit = true`, confirm a `.gputrace`
  file is written next to the binary, the trace opens in Xcode, and
  it contains the expected frame range (Init → Close). Confirm the
  file is *not* written when `captureOnInit = false` or when the
  debug layer is off.

## Rollout order

1. ✅ Slice landed: cross-cutting flags + macro + D3D12.
2. ✅ Vulkan §2.1, §2.2 (gate + log re-route). Smallest blast radius;
   exercises the same flag from a second backend.
3. ✅ Vulkan §2.3 (GPU-AV). Independent; shipped alongside §2.1/§2.2.
4. ✅ Vulkan §2.4 (object-name no-op). Landed as a centralized
   interposer in `VulkanExtStubs.cpp`, not per-callsite null-checks;
   pre-existing latent crash-on-null bug closed in the process.
5. ✅ Metal §3 v1 items: programmatic capture + env-var docs +
   stale-comment fix. The validation toggle was dropped (no Metal API).
   Landed together in `src/metal/GEMetal.mm` (constructor + new
   destructor), `OmegaGTE.cpp`/`GTEDevice.h` (the `captureOnInit` /
   `captureFilePath` plumbing), and `gte/docs/Metal-Debug.md`.
6. Future: Metal §3.1 items, in any order driven by need.
7. §4 engine-side resource logging — independent of §2/§3 and can
   interleave with them. Order within §4:
   a. §4.4 first — unify `ResourceTracking::Tracker` gating with
      `isDebugLayerEnabled()`. One small PR; zero behavior change
      for existing tracker users since the env var stays as
      override.
   b. §4.5/§4.6 — add `DEBUG_LOG`/`DEBUG_INFO`/… macros and the
      `GTEInitOptions::logLevel`/`logDomains` fields.
   c. §4.3 — backfill D3D12 and Vulkan tracker call sites + add
      `DEBUG_LOG` lines next to every tracker emit. This is the
      bulk-edit pass; ride it backend-by-backend.
   d. PIPELINE/SHADER/MEMORY/ASSET text coverage. Smallest pieces
      last, drop in opportunistically.

## 4. Engine-side resource logging

Native validation (D3D12 debug layer, Vulkan `VK_LAYER_KHRONOS_validation`)
tells you when the GPU API was misused. It does *not* tell you what
OmegaGTE is doing on its caller's behalf — what got created, what got
freed, why an allocation failed, what fence the queue is waiting on.
Today the engine logs a handful of failures via `DEBUG_STREAM` (mostly
error-on-failure) and nothing on success. When a user reports
"my texture is black", we have no log trace to walk.

### Existing infrastructure to fold in

There is already a structured resource-event recorder in
`src/common/GEResourceTracker.{h,cpp}`
(`OmegaGTE::ResourceTracking::Tracker`). It mints monotonic resource
IDs, records `Create | Destroy | Submit | Complete | Present |
ResizeRebuild | Bind | Unbind | Marker` events into a 4096-entry ring,
aggregates per-type churn metrics, and exposes
`recentEvents`, `churnMetricsSnapshot`, `dumpRecentTimeline`, and
`dumpChurnMetrics`. The Metal backend already emits events from
`GEMetal.mm`, `GEMetalTexture.mm`, `GEMetalCommandQueue.mm`, and
`GEMetalRenderTarget.mm`. D3D12 and Vulkan have *not* been wired in.

The tracker is gated on its own `OMEGAGTE_RESOURCE_TRACE` env var,
parallel to and independent from `isDebugLayerEnabled()`. That split
is the bug §4 fixes: one debug toggle should govern both surfaces.

§4 is therefore **not** a new tracker. It is:

1. Unify the tracker's gating with `isDebugLayerEnabled()`.
2. Add a thin text-emission layer (`DEBUG_LOG`/`DEBUG_INFO`/…) that
   sits *next to* the tracker — same call site emits a structured
   event *and* a human-readable line.
3. Cover the domains the tracker does not structurally model
   (`PIPELINE`, `SHADER`, `MEMORY`, `ASSET`) with text-only logging.
4. Backfill the D3D12 and Vulkan tracker call sites the Metal backend
   already has.

### 4.1. Decision points

1. **One axis or two for filtering?** Options:
   - **(a) Levels only** — `ERROR`, `INFO`, `TRACE`. Familiar from every
     other logger. Easy to wire as a single integer threshold. Loses
     the "show me only swapchain stuff" use case.
   - **(b) Domains only** — `RESOURCE`, `PIPELINE`, `QUEUE`,
     `RENDERTGT`, `MEMORY`, `SHADER`. Bitmask of enabled domains.
     Great for focused debugging; awkward when you want "show me all
     errors regardless of domain."
   - **(c) Both** — level *and* domain on every line, filter on either.
     Most flexible; most ceremony at every callsite.
   - **Recommended: (c) with sensible defaults.** Level + domain on
     every line, but a one-arg `DEBUG_STREAM(msg)` defaults to
     `INFO`/`GENERAL` so we don't rewrite the existing 50+ call sites
     unless we want to upgrade their tagging. New macros
     `DEBUG_LOG(level, domain, msg)`, plus convenience wrappers
     `DEBUG_ERROR(domain, msg)`, `DEBUG_TRACE(domain, msg)`.

2. **What controls visibility?** Today: two parallel switches —
   `isDebugLayerEnabled()` gates `DEBUG_STREAM`, `OMEGAGTE_RESOURCE_TRACE`
   gates the tracker. Proposed: `isDebugLayerEnabled()` is the master
   switch for both. Add `GTEInitOptions::logLevel`
   (`Error|Info|Trace`, default `Info`) and
   `GTEInitOptions::logDomains` (bitmask, default all). The
   `OMEGAGTE_RESOURCE_TRACE` env var stays as a one-way override
   ("force on even if `Init()` didn't opt in") for post-mortem repro
   — but the default path is the `GTEInitOptions` flag.

3. **Resource IDs — already solved.** The tracker's
   `Tracker::nextResourceId()` returns monotonic `uint64_t`s and is
   already stamped onto `traceResourceId` fields in the existing
   Metal backend code. `DEBUG_LOG` adopts the same IDs verbatim:
   when a class already has `traceResourceId`, the text formatter
   reads it; new sites call `nextResourceId()` at construction and
   stash it the same way. **No new `GTELoggable` base class** — the
   tracker's free function does the job without touching the
   inheritance tree. (Previous draft of this section proposed
   hoisting a base; withdrawn.)

4. **Line format.** Greppable beats pretty. Suggested:
   ```
   [GED3D12Engine_Internal] - <LEVEL> <DOMAIN> id=<n> name=<s|->: <message>
   ```
   The `id=` here is the *same* tracker ID, so a single `grep
   "id=42"` correlates text logs against tracker dumps.

5. **Hot-path cost.** `DEBUG_TRACE` calls will appear in
   command-buffer recording and `present()`. Even gated, the
   atomic-load-per-call adds up if we sprinkle them in the inner draw
   loop. Two mitigations:
   - Don't add `TRACE` inside command-list recording — only at
     command-buffer-level boundaries (begin, end, commit, completion).
     Same rule the tracker already follows (it has no `Bind`/`Unbind`
     emission inside record loops today, only at queue boundaries).
   - For the few sites that really need per-draw tracing,
     opt-in *compile-time* under `OMEGAGTE_DEBUG_TRACE_HOT` so release
     builds don't even branch.

### 4.2. Taxonomy

Domains (`DEBUG_DOMAIN_*` bits in a `uint32_t`):

| Domain      | What it covers |
|-------------|----------------|
| `GENERAL`   | Engine init, shutdown, anything that doesn't fit elsewhere. Default for the legacy one-arg `DEBUG_STREAM`. |
| `RESOURCE`  | `GEBuffer`, `GETexture`, `GEHeap`, `GESamplerState`, `GEFence`, `GEAccelerationStruct` lifecycle. |
| `PIPELINE`  | `GERenderPipelineState`, `GEComputePipelineState`, root signature / descriptor set layout creation, reflection results. |
| `SHADER`    | `GTEShader` / `GTEShaderLibrary` load, OmegaSL→backend translation, feature-rejection sentinels (already partly logged). |
| `QUEUE`     | `GECommandQueue` create, command buffer commit, fence signal/wait, drain, completion callbacks. |
| `RENDERTGT` | `GENativeRenderTarget` / `GETextureRenderTarget` create, swapchain format selection, resize, present. |
| `MEMORY`    | D3D12MA / VMA allocator pool creation, allocation failures with size/heap-type details, heap growth. |
| `ASSET`     | `GEMeshAsset` / `GETextureAsset` load, codec selection, format conversion. |

Levels:

| Level     | When to use |
|-----------|-------------|
| `ERROR`   | Operation failed; subsequent calls likely to misbehave. Existing failure-path `DEBUG_STREAM`s upgrade to this. |
| `INFO`    | Significant lifecycle event — resource created/destroyed, queue committed, swapchain resized. One-shot or once-per-frame at most. |
| `TRACE`   | Per-call internal detail — pipeline-state-cache hits/misses, descriptor-heap allocation, fence wait values. |

### 4.3. Coverage table

Two columns: whether the tracker already records a structured event
for the site (Metal-only today), and what the text-emission layer
should add. "Backfill D3D12/Vulkan" means: copy the Metal call
pattern (`Tracker::instance().emit(EventType::…, Backend::…, …)`) into
the matching D3D12/Vulkan site, then add the `DEBUG_LOG` line next to
it.

| Site | Level | Domain | Tracker event | New work |
|------|-------|--------|---------------|----------|
| `makeBuffer` | `INFO` | `RESOURCE` | `Create` (Metal ✓, D3D12/VK ✗) | Backfill D3D12/Vulkan + `DEBUG_INFO` line |
| `makeTexture` | `INFO` | `RESOURCE` | `Create` (Metal ✓, D3D12/VK ✗) | Backfill D3D12/Vulkan + `DEBUG_INFO` line |
| `makeHeap` | `INFO` | `RESOURCE` | not in tracker today | Add tracker event + `DEBUG_INFO` line |
| `makeFence` | `INFO` | `RESOURCE` | not in tracker today | Add tracker event + `DEBUG_INFO` line |
| `makeSamplerState` | `INFO` | `RESOURCE` | not in tracker today | Add tracker event + `DEBUG_INFO` line |
| `allocateAccelerationStructure` | `INFO` | `RESOURCE` | not in tracker today | Add tracker event + `DEBUG_INFO` line |
| `~GEBuffer` / `~GETexture` / etc. | `INFO` | `RESOURCE` | `Destroy` (Metal ✓, D3D12/VK ✗) | Backfill D3D12/Vulkan + `DEBUG_INFO` line |
| Allocation failure paths | `ERROR` | `RESOURCE`/`MEMORY` | not in tracker (errors are not events today) | `DEBUG_ERROR` line only; existing `DEBUG_STREAM` calls upgrade in place |
| `makeRenderPipelineState` / `makeComputePipelineState` | `INFO` | `PIPELINE` | out of scope for tracker | `DEBUG_INFO` line only |
| Pipeline compile failure | `ERROR` | `PIPELINE` | out of scope | `DEBUG_ERROR` line only |
| `makeCommandQueue` | `INFO` | `QUEUE` | `Create` (Metal ✓, D3D12/VK ✗) | Backfill + `DEBUG_INFO` line |
| `commit` / `commitAndWait` | `TRACE` | `QUEUE` | `Submit` (Metal ✓, D3D12/VK ✗) | Backfill + `DEBUG_TRACE` line |
| Command buffer completion | `TRACE` | `QUEUE` | `Complete` (Metal ✓, D3D12/VK ✗) | Backfill + `DEBUG_TRACE` line |
| `makeNativeRenderTarget` | `INFO` | `RENDERTGT` | `Create` (Metal ✓, D3D12/VK ✗) | Backfill + `DEBUG_INFO` line |
| Swapchain resize | `INFO` | `RENDERTGT` | `ResizeRebuild` (Metal ✓, D3D12/VK ✗) | Backfill + `DEBUG_INFO` line |
| `present()` | `TRACE` | `RENDERTGT` | `Present` (Metal ✓, D3D12/VK ✗) | Backfill + `DEBUG_TRACE` line |
| OmegaSL → backend shader compile | `TRACE` | `SHADER` | out of scope | `DEBUG_TRACE` line only |
| Feature-rejection sentinel | `ERROR` | `SHADER` | out of scope | Upgrade existing `DEBUG_STREAM` to `DEBUG_ERROR` |

Sites we explicitly do **not** add `TRACE` to:
- Inside command-list recording (`drawIndexed`, `setVertexBuffer`,
  `dispatch`, etc.). Hot path; see §4.1.5.
- D3D12 descriptor-heap entry creation per-bind. Same reason.
- Per-draw root-parameter binding.

### 4.4. Unifying with `ResourceTracking::Tracker`

Three changes inside `GEResourceTracker.{h,cpp}`, all small:

1. **Replace the env-var gate with the debug-layer flag.** Today
   `TrackerState` reads `OMEGAGTE_RESOURCE_TRACE` once at construction
   and caches into `traceEnabled`. Change `Tracker::enabled()` to
   return `traceEnabled || isDebugLayerEnabled()`. The env var stays
   as a one-way override (so post-mortem repros without an
   `OMEGAGTE_DEBUG` build still work), and the master switch picks it
   up for free.

2. **Add `Tracker::enabledForDomain(uint32_t)`** as a cheap predicate
   so the text-emission layer in §4.5 can skip formatting work when a
   domain is masked out, without touching the tracker's own emit path
   (which has its own domain assumption baked in via `EventType`).

3. **No schema changes to `Event` or `TypeChurnMetrics`.** The
   existing fields are sufficient; PIPELINE/SHADER/MEMORY/ASSET sites
   go through `DEBUG_LOG` only, not `emit`. If we later want
   structured pipeline-compile events, add a new `EventType` then —
   not now.

The `traceResourceId` field that already lives on `GEMetalTexture`,
`GEMetalCommandQueue`, etc., becomes the canonical resource ID for
the text formatter too. D3D12 and Vulkan classes that get backfilled
in §4.3 add the same field with the same name and same initialization
pattern.

### 4.5. Macros

Add to `gte/include/omegaGTE/GE.h`, next to the existing
`DEBUG_STREAM`:

```cpp
namespace OmegaGTE {
    enum class DebugLogLevel : uint8_t { Error = 0, Info = 1, Trace = 2 };
    constexpr uint32_t DEBUG_DOMAIN_GENERAL   = 1u << 0;
    constexpr uint32_t DEBUG_DOMAIN_RESOURCE  = 1u << 1;
    constexpr uint32_t DEBUG_DOMAIN_PIPELINE  = 1u << 2;
    constexpr uint32_t DEBUG_DOMAIN_SHADER    = 1u << 3;
    constexpr uint32_t DEBUG_DOMAIN_QUEUE     = 1u << 4;
    constexpr uint32_t DEBUG_DOMAIN_RENDERTGT = 1u << 5;
    constexpr uint32_t DEBUG_DOMAIN_MEMORY    = 1u << 6;
    constexpr uint32_t DEBUG_DOMAIN_ASSET     = 1u << 7;

    OMEGAGTE_EXPORT bool debugLogShouldEmit(DebugLogLevel, uint32_t domain);
    OMEGAGTE_EXPORT const char *debugLogLevelName(DebugLogLevel);
    OMEGAGTE_EXPORT const char *debugLogDomainName(uint32_t singleBit);
}

#define DEBUG_LOG(level, domain, message)                                      \
    do {                                                                       \
        if (::OmegaGTE::debugLogShouldEmit((level), (domain))) {               \
            std::cout << "[" << DEBUG_ENGINE_PREFIX << "] - "                  \
                      << ::OmegaGTE::debugLogLevelName(level) << " "           \
                      << ::OmegaGTE::debugLogDomainName(domain) << " "         \
                      << message << std::endl;                                 \
        }                                                                      \
    } while (0)

#define DEBUG_ERROR(domain, message) DEBUG_LOG(::OmegaGTE::DebugLogLevel::Error, domain, message)
#define DEBUG_INFO(domain, message)  DEBUG_LOG(::OmegaGTE::DebugLogLevel::Info,  domain, message)
#define DEBUG_TRACE(domain, message) DEBUG_LOG(::OmegaGTE::DebugLogLevel::Trace, domain, message)
```

`debugLogShouldEmit` is the single hot-path function: returns
`isDebugLayerEnabled() && level <= g_debugLogLevel && (g_debugLogDomains & domain)`.
All three fields are atomics loaded with `memory_order_acquire`. The
existing `DEBUG_STREAM(msg)` macro stays as-is — it implicitly maps to
`DEBUG_LOG(Info, GENERAL, msg)` semantically, and gradually-typed
upgrades to a more specific level/domain are opt-in.

`DEBUG_LOG` does *not* replace `Tracker::emit` — they live side by
side. At a resource creation site you call both: `Tracker::emit(...)`
records the structured event for `dumpRecentTimeline` /
`churnMetricsSnapshot`, and `DEBUG_INFO(RESOURCE, ...)` writes the
human-readable line. Both gate on the same master switch (§4.4
change 1), so turning the debug layer off silences both surfaces in
one step.

### 4.6. `GTEInitOptions` additions

```cpp
struct GTEInitOptions {
    DebugLayer debugLayer = DebugLayer::Default;
    bool gpuBasedValidation = false;

    // ── New ──────────────────────────────────────────────────────
    DebugLogLevel logLevel = DebugLogLevel::Info;
    uint32_t logDomains = ~0u;   // all domains enabled by default
};
```

Resolution in `OmegaGTE.cpp::resolveDebugFlags` extends to write these
into two new atomics consumed by `debugLogShouldEmit`.

### 4.7. Verification

- Run any existing sample with `logLevel = Trace` and confirm a
  create-line and destroy-line appear for every `GEBuffer` and
  `GETexture`, matched by `id=`.
- Run with `logDomains = DEBUG_DOMAIN_PIPELINE` and confirm only
  pipeline lines appear; resource and queue lines are silenced.
- Run with `debugLayer = Disabled` and confirm zero lines regardless
  of level/domain settings, *and* that `Tracker::recentEvents()`
  returns empty — confirms the unified gate from §4.4.
- Run with `debugLayer = Disabled` but `OMEGAGTE_RESOURCE_TRACE=1` in
  the env, confirm the tracker still records events (override path)
  while `DEBUG_LOG` text stays silent. Asserts the env-var override
  still works for post-mortem.
- `grep "id=<n>"` on a sample's log should produce the same `id`s
  that appear in `Tracker::dumpRecentTimeline()` — single ID space.
- Run an existing Metal sample, confirm
  `Tracker::dumpChurnMetrics()` still produces the same shape of
  output as before §4 landed (no schema break).

### 4.8. Out of scope for this section

- Structured log sinks (JSON, OTLP). The format is human + `grep`.
- Per-frame stats / counters (draw count, descriptor heap pressure,
  GPU timing). Belongs in a separate profiling story, not a logging
  one.
- Log destinations beyond `std::cout`. If a redirector is needed later,
  swap the macro body — the call sites don't change.

## Open questions

- **Should `GTEInitOptions` move into its own header** to avoid pulling
  `OmegaGTE.h` (which transitively pulls every backend forward-decl)
  into translation units that only need the options struct? Defer
  until a real caller hits the include cost.
- **`Open question above resolved by §4.`** Per-callsite verbosity is
  now level + domain. Keeping the bullet here so readers know the
  question was answered downstream, not abandoned.
