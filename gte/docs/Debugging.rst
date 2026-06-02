===============
Debugging OmegaGTE
===============

.. contents:: On this page
   :local:
   :depth: 2

----

When a frame looks wrong, a draw call silently misbehaves, or the engine
crashes inside a backend, the question is always the same: *what was the
GPU actually told to do?* This page is a tour of every debugging surface
OmegaGTE exposes today and the order in which to reach for them.


The Debug Layer Toggle
======================

OmegaGTE has a single process-wide toggle that controls every debug
surface in the engine — backend validation layers, the
:code:`DEBUG_STREAM` logging macro, and (Metal-only) programmatic API
validation and GPU capture. It is resolved exactly once at
:code:`Init()` and frozen for the process lifetime; nothing in the
engine re-reads it.

The toggle has two layers, in order of precedence:

1. **Per-call override** via :code:`GTEInitOptions::debugLayer`.
   :code:`Enabled` forces it on, :code:`Disabled` forces it off, and
   :code:`Default` (the default) defers to layer 2.
2. **Compile-time default** via the :code:`OMEGAGTE_DEBUG` CMake
   option. ON in :code:`Debug` and :code:`RelWithDebInfo` builds, OFF
   in :code:`Release` and :code:`MinSizeRel`.

.. code-block:: cpp

   // Default behaviour — follow the build configuration.
   auto gte = OmegaGTE::InitWithDefaultDevice();

   // Force the debug layer on for a Release build.
   OmegaGTE::GTEInitOptions opts;
   opts.debugLayer = OmegaGTE::GTEInitOptions::DebugLayer::Enabled;
   auto gte = OmegaGTE::InitWithDefaultDevice(opts);

   // Force the debug layer off for a perf-sensitive integration test
   // built in Debug.
   OmegaGTE::GTEInitOptions opts;
   opts.debugLayer = OmegaGTE::GTEInitOptions::DebugLayer::Disabled;
   auto gte = OmegaGTE::InitWithDefaultDevice(opts);

After :code:`Init()` returns, callers can query the resolved state at
any time:

.. code-block:: cpp

   if (OmegaGTE::isDebugLayerEnabled()) {
       // …extra diagnostic work…
   }

Turning the layer off does **not** strip log strings from the binary —
:code:`DEBUG_STREAM` runtime-checks the flag and stays a no-op when
off. The branch is a single atomic load that the CPU's predictor
handles for free in a hot loop, so the cost of leaving the macro
present in release code is effectively zero.


GPU-Based Validation
====================

Both Direct3D 12 and Vulkan support a second, much heavier validation
tier that *executes* on the GPU to catch out-of-bounds buffer access,
descriptor-table misuse, and other problems that purely CPU-side
validation can't see. It is gated by a separate flag because the cost
is significant — 5×–10× slower draw calls in typical workloads.

.. code-block:: cpp

   OmegaGTE::GTEInitOptions opts;
   opts.debugLayer = OmegaGTE::GTEInitOptions::DebugLayer::Enabled;
   opts.gpuBasedValidation = true;
   auto gte = OmegaGTE::InitWithDefaultDevice(opts);

:code:`gpuBasedValidation` is ignored when the debug layer is off.
Metal exposes a related capability via the
:code:`MTL_SHADER_VALIDATION=1` environment variable (macOS 14+); see
the Metal section below.


Engine Logging
==============

When the debug layer is on, every internal :code:`DEBUG_STREAM` call
in the engine becomes visible on :code:`stdout`. Each line is prefixed
with the engine that emitted it:

.. code-block:: text

   [GED3D12Engine_Internal] - Making D3D12RenderPipelineState
   [GED3D12Engine_Internal] - D3D12 [WARN id=123] Resource state mismatch on Subresource 0
   [GEMetalEngine_Internal] - GEMetalEngine Successfully Created
   [GEVulkanEngine_Internal] - Successfully Created GEVulkanEngine

The :code:`[D3D12 …]`, :code:`[VVL …]` (Vulkan Validation Layer), and
Metal-validation lines come from the native debug layer and are
funneled through the same macro, so a single grep covers everything.

Beyond the binary toggle, OmegaGTE is rolling out a richer
classification (levels :code:`Error|Info|Trace` × domains
:code:`RESOURCE|PIPELINE|QUEUE|RENDERTGT|SHADER|MEMORY|ASSET`) and
unification with the structured resource tracker described in the
next section. See :code:`gte/docs/Debug-Layer-Plan.md` for the
in-flight plan; this page will be updated as those macros land.


Structured Resource Tracking
============================

For questions the log can't easily answer — *was this texture ever
freed? how many command buffers are in flight? which queue holds the
fence we're waiting on?* — OmegaGTE includes a structured event
recorder at :code:`OmegaGTE::ResourceTracking::Tracker`.

It records create/destroy/submit/complete/present events into a
fixed-size ring (4096 entries), assigns each resource a monotonic
:code:`uint64_t` ID, and aggregates per-type churn metrics
(short-lifetime counts, startup-burst detection, average lifetime).

.. code-block:: cpp

   #include "common/GEResourceTracker.h"

   auto & tracker = OmegaGTE::ResourceTracking::Tracker::instance();

   // Dump the last 100 events as a human-readable timeline.
   std::cout << tracker.dumpRecentTimeline(100);

   // Dump per-type churn metrics — useful for spotting "we created
   // 50,000 textures in the first second" pathologies.
   std::cout << tracker.dumpChurnMetrics();

   // Or get a snapshot programmatically.
   auto snap = tracker.churnMetricsSnapshot();

The tracker is gated on either :code:`isDebugLayerEnabled()` or the
:code:`OMEGAGTE_RESOURCE_TRACE` environment variable, whichever is
set. The env-var path is intentional — it lets you record a trace
from a release-built binary in the field without rebuilding.

The Metal backend is fully wired today. D3D12 and Vulkan backfill is
on the roadmap; see the rollout order in
:code:`gte/docs/Debug-Layer-Plan.md`.


Per-Backend Notes
=================

D3D12
-----

When the debug layer is on:

* :code:`ID3D12Debug1::EnableDebugLayer` is invoked before device
  creation, with GPU-Based Validation gated separately on
  :code:`GTEInitOptions::gpuBasedValidation`.
* :code:`DXGI_CREATE_FACTORY_DEBUG` is set on the DXGI factory so
  swap-chain and adapter messages reach the debug runtime.
* :code:`ID3D12InfoQueue1::RegisterMessageCallback` (Windows 10
  21H1+ SDK) installs a callback that funnels every validation
  message into :code:`DEBUG_STREAM` with its severity, ID, and
  description.

On older Windows SDKs that don't expose :code:`ID3D12InfoQueue1`, the
callback registration silently no-ops; messages still reach the
debugger output stream via the debug layer's default sink, so they're
visible under the Visual Studio debugger.

Metal
-----

The Metal backend's debug behaviour splits across two mechanisms.

**Programmatic toggle (macOS 11+).** When the debug layer is on at
:code:`Init()`, OmegaGTE calls
:code:`MTLCaptureManager.sharedCaptureManager.shouldEnableValidation = YES`
before creating the first command queue. This mirrors D3D12's
ergonomics — no environment variable required.

**Environment variables (always available).** For older macOS targets
or when the caller constructs their own :code:`id<MTLDevice>` before
handing it to OmegaGTE, the standard Metal env vars still work:

.. list-table::
   :widths: 35 65
   :header-rows: 1

   * - Variable
     - Effect
   * - :code:`METAL_DEVICE_WRAPPER_TYPE=1`
     - Enable Metal API validation. Must be set before
       :code:`MTLCreateSystemDefaultDevice`.
   * - :code:`MTL_DEBUG_LAYER=1`
     - Extended API validation, equivalent to Xcode's scheme toggle.
   * - :code:`MTL_SHADER_VALIDATION=1`
     - Shader bounds checking and uninitialised-memory detection
       (macOS 14+).
   * - :code:`METAL_ERROR_MODE=3`
     - Print errors. :code:`5` aborts on first error — useful in CI.

In an Xcode scheme, set these under
**Edit Scheme → Run → Arguments → Environment Variables**.

**Programmatic GPU capture.** OmegaGTE can drive
:code:`MTLCaptureManager` to write a :code:`.gputrace` file across the
process lifetime when both :code:`debugLayer` and a separate
:code:`captureOnInit` flag are set. The trace opens in Xcode's
built-in GPU Frame Debugger for shader inspection, resource
timelines, and pixel history. Useful for CI repro artifacts; trace
files can grow to multiple gigabytes, which is why the flag is opt-in
even when the debug layer is on.

Vulkan
------

When the debug layer is on:

* :code:`VK_LAYER_KHRONOS_validation` is enabled at instance creation
  (if present in the loader's layer list).
* :code:`VK_EXT_debug_utils` is requested so validation messages and
  :code:`vkSetDebugUtilsObjectNameEXT` calls work.
* A :code:`VkDebugUtilsMessengerEXT` is installed and routes
  warnings/errors/info through :code:`DEBUG_STREAM` with a
  :code:`[VVL …]` prefix.
* With :code:`gpuBasedValidation`, the engine requests
  :code:`VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT` when the
  layer advertises :code:`VK_EXT_validation_features`. Silently
  downgrades to plain validation if the feature is missing.

**Validation layer version requirement.** OmegaGTE's Vulkan backend is
built against the LunarG SDK headers vendored at
:code:`gte/deps/vulkan_sdk/1.3.283.0`. The validation layer that
*runs* against the engine must come from the same SDK. If the loader
picks up an older system-package VVL — Ubuntu 22.04's
:code:`vulkan-validationlayers 1.3.204.1`, for example — every device
creation will emit a spurious
:code:`VUID-VkDeviceCreateInfo-pNext-pNext` error for any feature
struct added to the Vulkan spec after that VVL was cut. The driver
consumes the struct correctly; only the validation layer's allowlist
is out of date. A concrete case: when the device advertises
:code:`VK_EXT_mesh_shader`, the engine chains
:code:`VkPhysicalDeviceMeshShaderFeaturesEXT` (sType
:code:`1000328000`) into :code:`pCreateInfo->pNext` — VVL ≤1.3.225
flags it as unknown.

The vendored SDK already ships the matching layer. Source its
environment script in any shell from which you launch a debug-built
OmegaGTE app:

.. code-block:: shell

   source gte/deps/vulkan_sdk/1.3.283.0/setup-env.sh
   ./build/wtk/tests/SVGViewRenderTest   # or whatever you are debugging

:code:`setup-env.sh` sets :code:`VULKAN_SDK`, prepends the SDK's
:code:`lib/` to :code:`LD_LIBRARY_PATH` (so the loader itself is
1.3.283 too), and adds the SDK's :code:`explicit_layer.d/` to
:code:`VK_ADD_LAYER_PATH` so the 1.3.283 Khronos validation layer
wins over any older one registered system-wide. Confirm by running
:code:`vulkaninfo --summary` from the same shell —
:code:`VK_LAYER_KHRONOS_validation` should report
:code:`api_version 1.3.283`.

Keep this scoped to the debugging shell rather than exporting it from
:code:`~/.profile` / :code:`~/.zshrc`; the override shadows the system
loader for every Vulkan app launched from that shell, which is the
right behaviour for a focused debug session and the wrong behaviour
as a global default.

External tooling — :code:`vkconfig`, the LunarG SDK's API dump layer,
RenderDoc capture layers — works alongside this; they enable
themselves via their own loader paths and don't conflict with the
in-process messenger.


Picking the Right Tool
======================

A rough decision tree:

* **"Why doesn't this draw?"** — turn the debug layer on and read
  :code:`stdout`. Most state-mismatch and resource-binding bugs surface
  as a single :code:`[D3D12 ERROR …]`, :code:`[VVL ERROR …]`, or Metal
  validation line.
* **"Why is this slow?"** — turn the debug layer *off*. GPU-Based
  Validation in particular distorts timings by an order of magnitude.
  Use platform profilers (PIX, Nsight Graphics, Xcode GPU Capture,
  RenderDoc with its profiling overlay) on a release-mode build.
* **"Why did this texture leak?"** or **"why are we creating 10k
  buffers per frame?"** — use the resource tracker. Its churn
  metrics surface short-lifetime patterns and startup bursts that no
  single log line will catch.
* **"What did the GPU actually execute on frame N?"** — capture a
  GPU trace with the platform-native tool (PIX on Windows, Xcode
  Frame Capture or Metal Debugger on macOS, RenderDoc on Linux/Vulkan,
  Nsight Graphics for NVIDIA-specific deep dives).

GPU-Based Validation, the debug layer, and the resource tracker each
answer a different question. Reach for the one that matches the
symptom, not all three by default.


Limitations and Future Work
===========================

Today OmegaGTE is a *consumer* of every external GPU debugger:
external tools (RenderDoc, PIX, Nsight Graphics, Xcode's Metal
Debugger) capture our frames and expose their own UIs. We do not
ship a debugger of our own.

In the long term, once **WTK** (the Omega Widget Toolkit) is mature
enough to host a non-trivial application, we plan to build an
OmegaGTE-native capture-based debugger — call it **OmegaGPUCapture**.
The scope target:

* **Capture** every backend API call OmegaGTE issues (D3D12 / Metal /
  Vulkan), keyed against the OmegaGTE call that originated it.
  Capture lives in a portable file format that is *not* tied to one
  backend, so a frame captured on Metal can be replayed and inspected
  on a Windows machine running D3D12 (within feature-set limits).
* **Replay** captured frames against the live device, with the
  ability to skip, step, or substitute draws.
* **Inspect** resource timelines (the structured tracker is the
  groundwork for this — its event ring becomes the capture index),
  pipeline state at any given draw, shader source ↔ disassembly
  alongside the OmegaSL source, and pixel history for any rendered
  pixel.
* **Edit and re-run** shaders against a captured frame, the way
  RenderDoc's shader-edit and Nsight Graphics' shader-substitute
  workflows do — but in OmegaSL, not the per-backend HLSL/MSL/SPIR-V
  translation.

OmegaGPUCapture is explicitly downstream of WTK landing a usable
desktop-application story. Until then, the recommendation is to use
the platform-native debugger for visual frame inspection and the
mechanisms documented above for everything else. The architecture
choices we are making now — the structured resource tracker, the
unified debug-layer toggle, the planned level/domain log
classification — are deliberately shaped so that OmegaGPUCapture can
consume them when the time comes.
