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
- **`addCompletedHandler` failure inspection** on every command buffer.
  Pull `commandBuffer.error` and route into `DEBUG_ERROR(QUEUE, …)`
  from §4 once that lands. This is the canonical engine-side error
  surface (not Critical — GPU execution failures are engine-reported,
  not caller-contract violations; see §4 policy). The existing raw
  `NSLog` in `GEMetalCommandQueue.mm:1170` is the prototype and
  migrates here. **The error report should fire even with the debug
  layer off** for the subset that indicates caller misuse the GPU
  caught (resource-residency faults, etc.) — those upgrade to
  `DEBUG_CRITICAL(QUEUE, …)` when the `MTLCommandBufferError` code
  identifies them. The taxonomy split between Critical and Error here
  is the §4.3 Critical-audit's responsibility, not §3.1's.

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
7. ✅ Interim Metal NSLog sweep (2026-06-03). Per-frame command-buffer
   `NSLog`s in `src/metal/GEMetalCommandQueue.mm` routed through a
   new `GTE_NSLOG` macro gated on `isDebugLayerEnabled()`. Three
   sites left raw as Critical-class prototypes for §4: buffer role
   mismatch (L50), GPU command-buffer execution error (L1170),
   empty-present (L1302). `GTE_NSLOG` retires when §4 lands; the
   three raw sites migrate to `DEBUG_CRITICAL` (L50, L1302) and
   `DEBUG_ERROR(QUEUE, …)` (L1170 — engine reports GPU failure, not
   caller violation).
8. §4 engine-side API logging — independent of §2/§3 and can
   interleave with them. Order within §4:
   a. §4.4 first — unify `ResourceTracking::Tracker` gating with
      `isDebugLayerEnabled()` (Critical-bypass carve-out included).
      One small PR; zero behavior change for existing tracker users
      since the env var stays as override.
   b. §4.5/§4.6 — add `DEBUG_LOG`/`DEBUG_CRITICAL`/`DEBUG_INFO`/…
      macros and the `GTEInitOptions::logLevel`/`logDomains` fields.
      `DEBUG_CRITICAL` ships in this PR so subsequent §4.3 backfill
      can use it immediately.
   c. §4.3 — backfill D3D12 and Vulkan tracker call sites + add
      `DEBUG_LOG` / `DEBUG_CRITICAL` lines next to every tracker
      emit and every spec-validation check. This is the bulk-edit
      pass; ride it backend-by-backend, **starting with Metal** since
      the existing `GTE_NSLOG` and three raw-`NSLog` Critical
      prototypes are the migration anchor. Order within Metal:
      lift the three raw sites to `DEBUG_CRITICAL`/`DEBUG_ERROR`
      first, then rewrite the `GTE_NSLOG` sites to the typed macros,
      then retire `GTE_NSLOG`.
   d. Encoding-API spec checks across all backends (the per-bind /
      per-pass / per-draw Critical rows from §4.3). Each backend
      already has the validation code paths inline (Metal's
      `checkBufferRoleAgainstShader` etc.); this step swaps their
      reporting from `assert`/`NSLog` to `DEBUG_CRITICAL`.
   e. PIPELINE/SHADER/MEMORY/ASSET text coverage. Smallest pieces
      last, drop in opportunistically.

## 4. Engine-side API logging

Native validation (D3D12 debug layer, Vulkan `VK_LAYER_KHRONOS_validation`)
tells you when the GPU API was misused. It does *not* tell you what
OmegaGTE is doing on its caller's behalf — what got created, what got
freed, why an allocation failed, what fence the queue is waiting on,
whether the caller bound a uniform buffer to a storage slot, whether
they tried to draw without an active render pass. Today the engine logs
a handful of failures via `DEBUG_STREAM` (mostly error-on-failure) and
nothing on success. When a user reports "my texture is black", we have
no log trace to walk.

Scope expands here from the original "resource lifecycle" framing: this
section now covers the **whole public OmegaGTE API** — device-side
factories, command-buffer encoding, queue/sync, swapchain/drawable,
shader/pipeline compile, asset load — not just `make*` + `~*` pairs.

### Logging policy: gated vs. always-on

Two log classes, distinguished by *who* the log accuses:

1. **Engine-internal diagnostic.** "We allocated a 4 MiB heap." "Render
   pass started." "Command buffer completed in 1.3ms." These describe
   what the engine did. They're gated on `isDebugLayerEnabled()` — off
   in release by default, on when the caller opts in. The whole
   `DEBUG_LOG`/`DEBUG_INFO`/`DEBUG_TRACE` family (§4.5) sits in this
   class.

2. **Caller invalidated our spec.** "You bound a uniform<T> resource to
   a storage<T> slot." "You called `present()` with no enqueued command
   buffers." "Shader source failed to compile." "The PSO format
   disagrees with the render target." These accuse the caller of
   violating the API contract. They **must surface even when the debug
   layer is off**, because a release build that silently ignores
   misuse turns into a black-texture / silent-no-op bug report we
   can't diagnose. New macro `DEBUG_CRITICAL(domain, message)`
   (§4.5) bypasses the master gate.

   Critical is **not** a synonym for "fatal" or "engine failed." GPU
   driver errors, OOM after best-effort fallback, fence timeouts —
   those are engine-side reports and stay gated as `ERROR`. Critical
   is reserved for *caller-contract violations*: the API was used in a
   way the documented contract forbids.

   The Metal-side `GTE_NSLOG` macro that landed in
   `src/metal/GEMetal.h` is the gated-side precedent — when §4 lands,
   its Metal call sites are rewritten through `DEBUG_LOG`/
   `DEBUG_CRITICAL` and `GTE_NSLOG` retires. The three raw `NSLog`
   sites left untouched in `GEMetalCommandQueue.mm` (buffer role
   mismatch L50, GPU CB execution error L1170, empty-present L1302)
   are the Critical prototype — L50 and L1302 migrate to
   `DEBUG_CRITICAL`, L1170 migrates to `DEBUG_ERROR(QUEUE, …)` (engine
   reports GPU failure; not a caller violation).

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
   - **(a) Levels only** — `CRITICAL`, `ERROR`, `INFO`, `TRACE`.
     Familiar from every other logger. Easy to wire as a single integer
     threshold. Loses the "show me only swapchain stuff" use case.
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
   switch for both — **with one carve-out**: `CRITICAL`-level emits
   bypass the master gate (see point 6 below). Add
   `GTEInitOptions::logLevel` (`Critical|Error|Info|Trace`, default
   `Info`) and `GTEInitOptions::logDomains` (bitmask, default all). The
   `OMEGAGTE_RESOURCE_TRACE` env var stays as a one-way override
   ("force on even if `Init()` didn't opt in") for post-mortem repro
   — but the default path is the `GTEInitOptions` flag. `logLevel` is
   the **threshold for gated emits only**; Critical fires regardless of
   the threshold (you cannot silence spec-violation reports through
   `logLevel`).

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

6. **Critical bypasses the master gate.** Spec-violation reports
   (caller-contract failures) must surface in release builds with the
   debug layer off, because otherwise the symptom downstream — black
   texture, dropped draw, silent allocation failure — has no diagnostic
   trail. The same gate-bypass rule applies to `logLevel`: setting
   `logLevel = Error` does *not* silence `Critical`. The only way to
   suppress Critical is to fix the caller — that's the point.

   Critical is held to a tight bar to keep it useful: each Critical
   site is a documented API contract that the caller broke. Engine
   diagnostics (allocation pressure, GPU-side failures, fallbacks the
   engine took on the caller's behalf) are not Critical — those are
   `ERROR`/`INFO` and gated normally. If you can't write a one-line
   statement of which API contract the caller violated, it's not
   Critical.

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

| Level      | Gated? | When to use |
|------------|--------|-------------|
| `CRITICAL` | **No** — always emits | Caller violated a documented API contract. Resource bound to a slot of the wrong kind, encoding call made outside the required pass, present with no buffered work, shader source the caller supplied failed to compile, descriptor with out-of-range values. The accusation points at the caller, not the engine. |
| `ERROR`    | Yes | Engine-side operation failed; subsequent calls likely to misbehave. GPU command-buffer execution error, allocation failure after fallback, fence wait timed out, driver rejected a pipeline. Existing failure-path `DEBUG_STREAM`s upgrade to this. |
| `INFO`     | Yes | Significant lifecycle event — resource created/destroyed, queue committed, swapchain resized, render pass started/ended. One-shot or once-per-frame at most. |
| `TRACE`    | Yes | Per-call internal detail — pipeline-state-cache hits/misses, descriptor-heap allocation, fence wait values, per-bind log. Subject to the §4.1.5 hot-path rule. |

### 4.3. Coverage table

Four columns: level, domain, tracker event (whether the structured-event
tracker already records this — Metal-only today for the existing
sites), and new work. "Backfill D3D12/Vulkan" means: copy the Metal
call pattern (`Tracker::instance().emit(EventType::…, Backend::…, …)`)
into the matching D3D12/Vulkan site, then add the `DEBUG_LOG` line
next to it. **Crit** rows use `DEBUG_CRITICAL` and emit regardless of
the master gate (§4.1.6).

The table is grouped by API surface so it's auditable against the
public headers (`gte/include/omegaGTE/`).

#### Device factory APIs (`GTEDevice::make*`)

| Site | Level | Domain | Tracker event | New work |
|------|-------|--------|---------------|----------|
| `makeBuffer` (success) | `INFO` | `RESOURCE` | `Create` (Metal ✓, D3D12/VK ✗) | Backfill D3D12/Vulkan + `DEBUG_INFO` |
| `makeBuffer` — descriptor invalid (zero size, bad alignment, unsupported usage) | **`CRITICAL`** | `RESOURCE` | n/a | `DEBUG_CRITICAL` |
| `makeBuffer` — allocation failure (after fallback) | `ERROR` | `MEMORY` | n/a | `DEBUG_ERROR` |
| `makeTexture` (success) | `INFO` | `RESOURCE` | `Create` (Metal ✓, D3D12/VK ✗) | Backfill + `DEBUG_INFO` |
| `makeTexture` — descriptor invalid (zero extent, format unsupported on device, mip/array count out of range) | **`CRITICAL`** | `RESOURCE` | n/a | `DEBUG_CRITICAL` |
| `makeTexture` — allocation failure | `ERROR` | `MEMORY` | n/a | `DEBUG_ERROR` |
| `makeHeap` (success) | `INFO` | `RESOURCE` | add tracker event | Tracker + `DEBUG_INFO` |
| `makeHeap` — descriptor invalid | **`CRITICAL`** | `RESOURCE` | n/a | `DEBUG_CRITICAL` |
| `makeFence` | `INFO` | `RESOURCE` | add tracker event | Tracker + `DEBUG_INFO` |
| `makeSamplerState` (success) | `INFO` | `RESOURCE` | add tracker event | Tracker + `DEBUG_INFO` |
| `makeSamplerState` — descriptor invalid (anisotropy out of range, address mode unsupported) | **`CRITICAL`** | `RESOURCE` | n/a | `DEBUG_CRITICAL` |
| `makeRenderPipelineState` (success) | `INFO` | `PIPELINE` | out of scope | `DEBUG_INFO` |
| `makeRenderPipelineState` — shader stage missing, incompatible vertex layout, RT format mismatch with descriptor | **`CRITICAL`** | `PIPELINE` | n/a | `DEBUG_CRITICAL` |
| `makeRenderPipelineState` — driver compile failure | `ERROR` | `PIPELINE` | n/a | `DEBUG_ERROR` |
| `makeComputePipelineState` (success) | `INFO` | `PIPELINE` | out of scope | `DEBUG_INFO` |
| `makeComputePipelineState` — kernel missing / threadgroup size unsupported | **`CRITICAL`** | `PIPELINE` | n/a | `DEBUG_CRITICAL` |
| `makeBlitPipelineState` / `makeMeshPipelineState` (success) | `INFO` | `PIPELINE` | out of scope | `DEBUG_INFO` |
| Mesh pipeline — mesh-shader feature not supported by device | **`CRITICAL`** | `PIPELINE` | n/a | `DEBUG_CRITICAL` |
| `makeNativeRenderTarget` (success) | `INFO` | `RENDERTGT` | `Create` (Metal ✓, D3D12/VK ✗) | Backfill + `DEBUG_INFO` |
| `makeNativeRenderTarget` — descriptor invalid (zero drawable size, format not supported by surface) | **`CRITICAL`** | `RENDERTGT` | n/a | `DEBUG_CRITICAL` |
| `makeTextureRenderTarget` | `INFO` | `RENDERTGT` | add tracker event | Tracker + `DEBUG_INFO` |
| `makeCommandQueue` | `INFO` | `QUEUE` | `Create` (Metal ✓, D3D12/VK ✗) | Backfill + `DEBUG_INFO` |
| `allocateAccelerationStructure` (success) | `INFO` | `RESOURCE` | add tracker event | Tracker + `DEBUG_INFO` |
| `allocateAccelerationStructure` — descriptor invalid (geometry references freed buffer, conflicting flags) | **`CRITICAL`** | `RESOURCE` | n/a | `DEBUG_CRITICAL` |

#### Shader & library APIs

| Site | Level | Domain | Tracker event | New work |
|------|-------|--------|---------------|----------|
| `GTEShaderLibrary::load` (success) | `INFO` | `SHADER` | out of scope | `DEBUG_INFO` |
| OmegaSL → backend shader translate (success) | `TRACE` | `SHADER` | out of scope | `DEBUG_TRACE` |
| Caller-supplied shader source — parse / compile error | **`CRITICAL`** | `SHADER` | n/a | `DEBUG_CRITICAL` |
| OmegaSL feature rejection (caller used feature unsupported by target) | **`CRITICAL`** | `SHADER` | n/a | Upgrade existing `DEBUG_STREAM` to `DEBUG_CRITICAL` |
| Backend compile error reported by driver | `ERROR` | `SHADER` | n/a | `DEBUG_ERROR` |
| Shader function not found in library (caller asked for missing name) | **`CRITICAL`** | `SHADER` | n/a | `DEBUG_CRITICAL` |

#### Command-buffer encoding APIs

These are per-frame; respect §4.1.5 hot-path rule. `TRACE` only at
pass boundaries, not inside the record loop.

| Site | Level | Domain | Tracker event | New work |
|------|-------|--------|---------------|----------|
| `startRenderPass` (success) | `TRACE` | `QUEUE` | add `RenderPassBegin` event (extends `EventType`) | Tracker + `DEBUG_TRACE` |
| `startRenderPass` — render target not bound, descriptor missing required attachment, format mismatch with PSO | **`CRITICAL`** | `RENDERTGT` | n/a | `DEBUG_CRITICAL` |
| `endRenderPass` | `TRACE` | `QUEUE` | add `RenderPassEnd` event | Tracker + `DEBUG_TRACE` |
| `startComputePass` / `endComputePass` | `TRACE` | `QUEUE` | add events | Tracker + `DEBUG_TRACE` |
| `startBlitPass` / `endBlitPass` | `TRACE` | `QUEUE` | add events | Tracker + `DEBUG_TRACE` |
| `setRenderPipelineState` (success) | `TRACE` | `PIPELINE` | out of scope | `DEBUG_TRACE` |
| `setRenderPipelineState` — called outside a render pass | **`CRITICAL`** | `PIPELINE` | n/a | `DEBUG_CRITICAL` |
| `setComputePipelineState` — called outside a compute pass | **`CRITICAL`** | `PIPELINE` | n/a | `DEBUG_CRITICAL` |
| `bindResourceAt{Vertex,Fragment,Compute,Mesh,Task}Shader` — bind kind mismatch (uniform vs storage, texture vs buffer, sampler at non-sampler slot) | **`CRITICAL`** | `RESOURCE` | n/a | `DEBUG_CRITICAL` (Metal L50/L720/L787 etc. become this) |
| `bindResourceAt…` — called outside the matching pass (vertex bind without active render pass; compute bind without active compute pass) | **`CRITICAL`** | `RESOURCE` | n/a | `DEBUG_CRITICAL` |
| `bindResourceAt…` — location not in shader's layout table | **`CRITICAL`** | `RESOURCE` | n/a | `DEBUG_CRITICAL` |
| `bindResourceAt…` (success) | — | — | n/a | Omit per §4.1.5 hot-path rule (compile-time `OMEGAGTE_DEBUG_TRACE_HOT` only) |
| `draw` / `drawIndexed` / `drawIndirect` / `drawIndexedIndirect` (success) | — | — | n/a | Omit per hot-path rule |
| `draw*` — no pipeline bound, index buffer missing for indexed variant, vertex count zero | **`CRITICAL`** | `QUEUE` | n/a | `DEBUG_CRITICAL` |
| `dispatch` / `dispatchIndirect` / `dispatchMesh` (success) | — | — | n/a | Omit per hot-path rule |
| `dispatch*` — no compute/mesh pipeline bound, threadgroup count zero, exceeds device max | **`CRITICAL`** | `QUEUE` | n/a | `DEBUG_CRITICAL` |
| `copyBuffer` / `copyTexture` (success) | `TRACE` | `RESOURCE` | add `Copy` event | Tracker + `DEBUG_TRACE` |
| `copyBuffer` — overlapping source/dest with same storage, out-of-bounds region | **`CRITICAL`** | `RESOURCE` | n/a | `DEBUG_CRITICAL` |
| `copyTexture` — format incompatible, mip/slice out of range, region exceeds texture extent | **`CRITICAL`** | `RESOURCE` | n/a | `DEBUG_CRITICAL` |
| `fillBuffer` (success) | `TRACE` | `RESOURCE` | n/a | `DEBUG_TRACE` |
| `fillBuffer` — 32-bit pattern not byte-uniform (Metal blit constraint; current `GEMetalCommandQueue.mm` L393 warning) | `INFO` | `RESOURCE` | n/a | `DEBUG_INFO` (engine fallback, not caller misuse — kept as INFO) |
| Encoded `signalFence` / `waitForFence` | `TRACE` | `QUEUE` | extend `Submit`/add fence event | Tracker + `DEBUG_TRACE` |
| `waitForFence` — fence destroyed before wait reaches GPU | **`CRITICAL`** | `QUEUE` | n/a | `DEBUG_CRITICAL` |
| `setViewport` / `setScissor` — outside an active render pass, or values outside RT bounds | **`CRITICAL`** | `RENDERTGT` | n/a | `DEBUG_CRITICAL` |
| `setPushConstants` — size exceeds shader's declared `constant<T>` slot, offset misaligned | **`CRITICAL`** | `PIPELINE` | n/a | `DEBUG_CRITICAL` |
| `_commit` (success) | `TRACE` | `QUEUE` | `Submit` (Metal ✓, D3D12/VK ✗) | Backfill + `DEBUG_TRACE` |
| Command-buffer completion (success) | `TRACE` | `QUEUE` | `Complete` (Metal ✓, D3D12/VK ✗) | Backfill + `DEBUG_TRACE` |
| Command-buffer completion (GPU execution error) | `ERROR` | `QUEUE` | n/a (errors not events) | `DEBUG_ERROR` (Metal L1170 migrates here) |

#### Queue / submit APIs (`GECommandQueue::*`)

| Site | Level | Domain | Tracker event | New work |
|------|-------|--------|---------------|----------|
| `submitCommandBuffer` (success) | `TRACE` | `QUEUE` | `Submit` (Metal ✓, D3D12/VK ✗) | Backfill + `DEBUG_TRACE` |
| `submitCommandBuffer` — buffer not in completed-encoding state, fence already signalled past requested value | **`CRITICAL`** | `QUEUE` | n/a | `DEBUG_CRITICAL` |
| `commitToGPU` (success) | `TRACE` | `QUEUE` | n/a | `DEBUG_TRACE` |
| `commitToGPUAndPresent` — no enqueued command buffers (current Metal L1302) | **`CRITICAL`** | `QUEUE` | n/a | `DEBUG_CRITICAL` |
| `commitToGPUAndWait` — timeout exceeded | `ERROR` | `QUEUE` | n/a | `DEBUG_ERROR` |
| `notifyCommandBuffer` — wait fence already destroyed | **`CRITICAL`** | `QUEUE` | n/a | `DEBUG_CRITICAL` |

#### Render target / swapchain APIs

| Site | Level | Domain | Tracker event | New work |
|------|-------|--------|---------------|----------|
| `acquireDrawable` (success) | `TRACE` | `RENDERTGT` | add `DrawableAcquire` event | Tracker + `DEBUG_TRACE` |
| `acquireDrawable` — called twice without `present`/release, surface lost | **`CRITICAL`** / `ERROR` | `RENDERTGT` | n/a | Critical for double-acquire (caller); Error for surface-lost (engine) |
| `presentToScreen` (success) | `TRACE` | `RENDERTGT` | `Present` (Metal ✓, D3D12/VK ✗) | Backfill + `DEBUG_TRACE` |
| `presentToScreen` — drawable not acquired this frame | **`CRITICAL`** | `RENDERTGT` | n/a | `DEBUG_CRITICAL` |
| Swapchain resize (success) | `INFO` | `RENDERTGT` | `ResizeRebuild` (Metal ✓, D3D12/VK ✗) | Backfill + `DEBUG_INFO` |
| Swapchain format negotiation — caller-requested format unsupported by surface | **`CRITICAL`** | `RENDERTGT` | n/a | `DEBUG_CRITICAL` |
| Swapchain backend rebuild (driver-initiated) | `INFO` | `RENDERTGT` | `ResizeRebuild` (Metal ✓, D3D12/VK ✗) | Backfill + `DEBUG_INFO` |

#### Resource lifecycle (destructors / map / unmap)

| Site | Level | Domain | Tracker event | New work |
|------|-------|--------|---------------|----------|
| `~GEBuffer` / `~GETexture` / `~GEHeap` / `~GEFence` / `~GESamplerState` / `~GENativeRenderTarget` / `~GETextureRenderTarget` / `~GEAccelerationStruct` / `~GECommandQueue` | `INFO` | `RESOURCE`/`QUEUE`/`RENDERTGT` | `Destroy` (Metal ✓, D3D12/VK ✗ for most) | Backfill + `DEBUG_INFO` |
| Destructor called while resource still referenced by an in-flight command buffer | **`CRITICAL`** | `RESOURCE` | n/a | `DEBUG_CRITICAL` |
| `GEBuffer::map` / `GETexture::map` (success) | `TRACE` | `RESOURCE` | add `Map`/`Unmap` events | Tracker + `DEBUG_TRACE` |
| `map` — storage is `GPUOnly`, range out of bounds, already mapped, mapped while GPU writing | **`CRITICAL`** | `RESOURCE` | n/a | `DEBUG_CRITICAL` |
| `unmap` — not currently mapped | **`CRITICAL`** | `RESOURCE` | n/a | `DEBUG_CRITICAL` |
| `GETexture::writeRegion` — region exceeds texture extent, mip/slice out of range | **`CRITICAL`** | `RESOURCE` | n/a | `DEBUG_CRITICAL` |

#### Asset APIs (`GEMeshAsset` / `GETextureAsset`)

| Site | Level | Domain | Tracker event | New work |
|------|-------|--------|---------------|----------|
| Mesh asset load (success) | `INFO` | `ASSET` | out of scope | `DEBUG_INFO` |
| Mesh asset load — file unreadable, format unrecognized, codec unsupported | **`CRITICAL`** | `ASSET` | n/a | `DEBUG_CRITICAL` (caller asked for an asset we can't serve) |
| Texture asset load (success) | `INFO` | `ASSET` | out of scope | `DEBUG_INFO` |
| Texture asset load — format conversion fallback taken | `INFO` | `ASSET` | n/a | `DEBUG_INFO` (engine took the fallback; not caller misuse) |
| Texture asset load — file unreadable / decode failed | **`CRITICAL`** | `ASSET` | n/a | `DEBUG_CRITICAL` |

#### Engine init / device APIs

| Site | Level | Domain | Tracker event | New work |
|------|-------|--------|---------------|----------|
| `Init()` (success) | `INFO` | `GENERAL` | n/a | `DEBUG_INFO` |
| `Init()` — Metal not supported on this device (existing `GEMetal.mm:1017`) | **`CRITICAL`** | `GENERAL` | n/a | `DEBUG_CRITICAL` (upgrade existing raw `NSLog`) |
| `Init()` — Vulkan instance/device creation failed | `ERROR` | `GENERAL` | n/a | `DEBUG_ERROR` |
| `Init()` — caller requested `gpuBasedValidation` without `debugLayer` | **`CRITICAL`** | `GENERAL` | n/a | `DEBUG_CRITICAL` (ignored, but flag the contract violation) |
| `Close()` — called twice / called before `Init()` | **`CRITICAL`** | `GENERAL` | n/a | `DEBUG_CRITICAL` |

Sites we explicitly do **not** add `TRACE` to:
- Inside command-list recording (`drawIndexed`, `setVertexBuffer`,
  `dispatch`, etc.) on the success path. Hot path; see §4.1.5.
- D3D12 descriptor-heap entry creation per-bind. Same reason.
- Per-draw root-parameter binding.

`CRITICAL` sites are *not* exempt from the hot-path rule by virtue of
being Critical — they're exempt because they're failure paths and the
branch is overwhelmingly not taken in steady state. The cost on the
success path is one untaken conditional, same as `TRACE`-gated code.

### 4.4. Unifying with `ResourceTracking::Tracker`

Three changes inside `GEResourceTracker.{h,cpp}`, all small:

1. **Replace the env-var gate with the debug-layer flag.** Today
   `TrackerState` reads `OMEGAGTE_RESOURCE_TRACE` once at construction
   and caches into `traceEnabled`. Change `Tracker::enabled()` to
   return `traceEnabled || isDebugLayerEnabled()`. The env var stays
   as a one-way override (so post-mortem repros without an
   `OMEGAGTE_DEBUG` build still work), and the master switch picks it
   up for free.

   **Tracker is gated; Critical text-emits are not.** A
   `DEBUG_CRITICAL` line fires regardless of `Tracker::enabled()`,
   but the tracker itself stays gated — emitting a structured
   `Event` into the ring buffer on every spec violation in a release
   build would silently grow per-frame allocation cost on a code path
   we can't measure. The Critical text-emit alone is enough to point
   the user at the offending call site; if they want the structured
   timeline around it, they enable the debug layer and reproduce.

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
    // Ordered low → high. CRITICAL is the sentinel that bypasses the
    // master gate (see debugLogShouldEmit).
    enum class DebugLogLevel : uint8_t {
        Critical = 0,
        Error    = 1,
        Info     = 2,
        Trace    = 3,
    };
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

#define DEBUG_CRITICAL(domain, message) DEBUG_LOG(::OmegaGTE::DebugLogLevel::Critical, domain, message)
#define DEBUG_ERROR(domain, message)    DEBUG_LOG(::OmegaGTE::DebugLogLevel::Error,    domain, message)
#define DEBUG_INFO(domain, message)     DEBUG_LOG(::OmegaGTE::DebugLogLevel::Info,     domain, message)
#define DEBUG_TRACE(domain, message)    DEBUG_LOG(::OmegaGTE::DebugLogLevel::Trace,    domain, message)
```

`debugLogShouldEmit` is the single hot-path function. Two branches:

```cpp
bool debugLogShouldEmit(DebugLogLevel lvl, uint32_t domain) {
    if (lvl == DebugLogLevel::Critical) {
        // Bypass the master gate. Domain mask still applies so a
        // caller who wants spec-violation reports for only one
        // subsystem can still narrow — but they cannot silence them
        // by turning the debug layer off.
        return (g_debugLogDomains.load(std::memory_order_acquire) & domain) != 0;
    }
    return isDebugLayerEnabled()
        && static_cast<uint8_t>(lvl) <= g_debugLogLevel.load(std::memory_order_acquire)
        && (g_debugLogDomains.load(std::memory_order_acquire) & domain) != 0;
}
```

The level ordinals are reversed from "intuitive" (Critical=0, Trace=3)
so `lvl <= g_debugLogLevel` reads as "this severity is at least as
important as the configured floor." Critical is the special case
that's checked first and short-circuits the master gate.

The default `g_debugLogDomains` is `~0u` (all domains), so the
Critical path is "on" out of the box and only the explicit caller
who masks out a domain can suppress the Critical report for that
domain. Test rigs that want to assert "no Critical fires under
this workload" can wire a counter into the same function and check it
post-run.

All three globals (`isDebugLayerEnabled()` backing flag,
`g_debugLogLevel`, `g_debugLogDomains`) are atomics loaded with
`memory_order_acquire`. The existing `DEBUG_STREAM(msg)` macro stays
as-is — it implicitly maps to `DEBUG_LOG(Info, GENERAL, msg)`
semantically, and gradually-typed upgrades to a more specific
level/domain are opt-in.

`DEBUG_LOG` does *not* replace `Tracker::emit` — they live side by
side. At a resource creation site you call both: `Tracker::emit(...)`
records the structured event for `dumpRecentTimeline` /
`churnMetricsSnapshot`, and `DEBUG_INFO(RESOURCE, ...)` writes the
human-readable line. Both gate on the same master switch (§4.4
change 1), so turning the debug layer off silences both gated
surfaces in one step — and leaves the Critical text-emit path alive
(§4.4 change 1 carve-out).

### 4.6. `GTEInitOptions` additions

```cpp
struct GTEInitOptions {
    DebugLayer debugLayer = DebugLayer::Default;
    bool gpuBasedValidation = false;

    // ── New ──────────────────────────────────────────────────────
    // Threshold for gated emits (Error/Info/Trace). Critical bypasses
    // this — see §4.1.6.
    DebugLogLevel logLevel = DebugLogLevel::Info;
    // Bitmask of DEBUG_DOMAIN_* bits to allow. Applies to gated *and*
    // Critical emits — masking a domain out suppresses Critical
    // reports for that subsystem too. Default ~0u (all domains).
    uint32_t logDomains = ~0u;
};
```

Resolution in `OmegaGTE.cpp::resolveDebugFlags` extends to write these
into two new atomics consumed by `debugLogShouldEmit`. `logLevel` is
clamped to `Error..Trace` when written — `Critical` as a configured
floor would be a no-op (Critical always fires) and would silently
silence `Error`/`Info`/`Trace`, which is almost certainly not what
the caller meant. Writing `Critical` is rejected with a
`DEBUG_CRITICAL(GENERAL, …)` complaint and the value clamped up to
`Error`. (Yes — Critical reports its own misconfiguration. The
self-reference is intentional: it's the one path that's always
visible.)

### 4.7. Verification

#### Gated path (Error/Info/Trace)

- Run any existing sample with `logLevel = Trace` and confirm a
  create-line and destroy-line appear for every `GEBuffer` and
  `GETexture`, matched by `id=`.
- Run with `logDomains = DEBUG_DOMAIN_PIPELINE` and confirm only
  pipeline lines appear; resource and queue lines are silenced.
- Run with `debugLayer = Disabled` and confirm zero **gated** lines
  regardless of level/domain settings, *and* that
  `Tracker::recentEvents()` returns empty — confirms the unified gate
  from §4.4.
- Run with `debugLayer = Disabled` but `OMEGAGTE_RESOURCE_TRACE=1` in
  the env, confirm the tracker still records events (override path)
  while gated `DEBUG_LOG` text stays silent. Asserts the env-var
  override still works for post-mortem.
- `grep "id=<n>"` on a sample's log should produce the same `id`s
  that appear in `Tracker::dumpRecentTimeline()` — single ID space.
- Run an existing Metal sample, confirm
  `Tracker::dumpChurnMetrics()` still produces the same shape of
  output as before §4 landed (no schema break).

#### Critical bypass

The whole point of the Critical level is that turning the debug layer
off does *not* silence spec-violation reports. Verify:

- Build with `-DOMEGAGTE_DEBUG=OFF` and `debugLayer = Disabled`.
  Deliberately bind a `Uniform`-role `GEBuffer` to a slot the shader
  declared as `buffer<T>` storage. Confirm the line
  `[<Engine>_Internal] - CRITICAL RESOURCE …` reaches stdout.
  Same workload with the binding corrected: zero CRITICAL lines.
- Same build, call `commitToGPUAndPresent` with an empty queue.
  Confirm CRITICAL fires. Buffer one CB and re-run: silent.
- Same build, mask the violating domain out via
  `logDomains = ~DEBUG_DOMAIN_RESOURCE`. Confirm the buffer-role
  Critical no longer fires (domain mask applies to Critical too),
  while a pipeline-domain Critical (PSO format mismatch) does fire.
  This is the documented suppression knob — masking a domain
  acknowledges "I know this surface misuses the API, hide it."
- Confirm the gated surfaces stay silent throughout (no `INFO`/
  `TRACE` lines, empty `Tracker::recentEvents()`).
- Attempt to set `GTEInitOptions::logLevel = DebugLogLevel::Critical`
  at `Init()`. Confirm `resolveDebugFlags` rejects with a CRITICAL
  self-report and clamps to `Error`.

#### Critical taxonomy audit

Independent of runtime: walk the §4.3 table and confirm every
`CRITICAL` row identifies a documented caller-contract violation
(not an engine-side failure). If a row reads "the driver returned
an error" or "the allocation pool was exhausted," it's misclassified
and should be `ERROR`. This audit lives in code review for the
landing PR, not in CI.

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
