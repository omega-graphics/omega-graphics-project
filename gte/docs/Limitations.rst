================================
Feature & Driver Limitations
================================

.. contents:: On this page
   :local:
   :depth: 2

----

Why OmegaGTE Has Limitations at All
------------------------------------

OmegaGTE is a thin abstraction layer on top of three native graphics APIs:
**Direct3D 12** on Windows, **Metal** on macOS and iOS, and **Vulkan** on
Linux and Android. It does not emulate features that the underlying API does
not provide. If Metal does not expose a particular GPU feature, OmegaGTE
cannot expose it on macOS either, no matter how cleanly it can be expressed
on Direct3D 12 or Vulkan.

This is a deliberate choice. An abstraction that pretends every backend can
do everything would have to either:

* Implement the missing feature in software on the platforms that lack it,
  which silently turns a fast hardware path into a slow CPU fallback, or
* Refuse to compile programs that use the feature on platforms that lack it,
  which moves the cross-platform problem from "runtime surprise" to "build
  configuration nightmare".

OmegaGTE chooses neither. Instead, the API surface reflects the **intersection
of what the three target backends can actually do**, with capability flags
that let an application query at runtime whether an optional feature is
available on the current device. When you write a renderer against OmegaGTE,
you are writing against a shared subset of D3D12, Metal, and Vulkan. The
shared subset is large and modern — it is far above OpenGL ES 3.0 — but it
is not the full feature set of any individual backend.

This document lists the places where the three backends diverge, what
OmegaGTE does about each one, and the driver quirks the engine has had to
work around in practice.

----

How OmegaGTE Reports Capabilities
----------------------------------

The ``GTEDevice`` interface exposes a ``capabilities()`` accessor that
returns a structure listing which optional features are available on the
current physical device. An application that wants to use ray tracing,
mesh shaders, sparse resources, sampler feedback, or any other optional
feature should query this structure first and choose a fallback path if the
feature is not present.

The capability bits are computed at device creation time by querying the
underlying API:

* On D3D12, by calling ``CheckFeatureSupport`` for the relevant
  ``D3D12_FEATURE_*`` enumeration values.
* On Metal, by reading the ``MTLGPUFamily`` and feature-set enumerations from
  the ``MTLDevice``.
* On Vulkan, by reading ``vkGetPhysicalDeviceFeatures2`` and the relevant
  feature structure chain.

A capability bit is set only when **all three layers** — the API, the
driver, and the hardware — support the feature. A feature that is exposed
by the API but unsupported by the driver on the current device, or
unsupported by the hardware itself, will report as unavailable.

----

Ray Tracing
------------

Hardware-accelerated ray tracing is the largest single area where backend
parity is incomplete.

**Direct3D 12** supports ray tracing through DirectX Raytracing (DXR) on
hardware that exposes the appropriate feature tier. The full pipeline —
acceleration structure builds, ray dispatch, hit shaders, miss shaders,
and any-hit / closest-hit semantics — is available on all current discrete
GPUs from NVIDIA, AMD, and Intel.

**Vulkan** supports ray tracing through the ``VK_KHR_acceleration_structure``
and ``VK_KHR_ray_tracing_pipeline`` extensions. Coverage is broad on modern
discrete cards but uneven on integrated GPUs and older drivers. Some Linux
distributions ship driver versions that pre-date the KHR ratification of the
extension and require a driver update to enable ray tracing.

**Metal** supports ray tracing on Apple silicon (M1 and later) and on
discrete AMD GPUs paired with Intel Macs running macOS 11 or later. The
feature is unavailable on older Intel Macs with integrated Intel graphics.
On iOS, ray tracing is supported on A13 and later. On older iOS devices the
ray tracing API is not callable at all.

OmegaGTE exposes ray tracing through a unified API but reports
``GTEDeviceCapabilities::supportsRayTracing`` as false on devices where any
of the three layers below it is missing the feature. Applications that wish
to use ray tracing should always check this flag before attempting to build
acceleration structures or dispatch rays.

----

Mesh Shaders
-------------

Mesh shaders replace the traditional vertex / geometry / tessellation
pipeline with a more direct compute-style geometry programming model. They
are useful for procedural geometry, fine-grained culling, and meshlet-based
rendering of very large scenes.

**Direct3D 12** supports mesh shaders on hardware at Shader Model 6.5 or
above (NVIDIA Turing and later, AMD RDNA 2 and later, Intel Arc).

**Vulkan** supports mesh shaders through ``VK_EXT_mesh_shader``, which is
broadly available on modern desktop drivers. On mobile and integrated GPUs
the extension is sparse.

**Metal** added object and mesh shaders in Metal 3 (macOS 13 / iOS 16) on
Apple silicon only. Intel Macs with discrete AMD GPUs and any Apple device
older than the Metal 3 cutoff cannot use mesh shaders.

OmegaGTE's mesh shader path is reported through
``supportsMeshShaders`` and is one of the optional features most likely to
be unavailable on any given device. The conventional vertex / fragment
pipeline is always available and should be the default code path.

----

Compute Subgroup Operations
----------------------------

Compute subgroup (also called "wave" or "SIMD-group") operations let threads
within a hardware execution unit share data without going through shared
memory. They are the basis for fast reductions, prefix sums, and
cooperative algorithms.

The three backends expose them with different feature levels:

* **D3D12** exposes wave intrinsics from Shader Model 6.0. Wave size is
  reported by ``D3D12_FEATURE_DATA_D3D12_OPTIONS1::WaveLaneCountMin`` and
  varies by vendor (NVIDIA: 32, AMD: 64 historically, 32 or 64 on RDNA).
* **Metal** exposes SIMD-group operations on all Apple silicon families
  and on most discrete AMD GPUs from MSL 2.1.
* **Vulkan** exposes subgroup operations through the
  ``VK_KHR_shader_subgroup_*`` family of extensions. Coverage is good but
  the feature mask and supported operations vary by driver.

OmegaSL provides ``subgroup_*`` intrinsics that map to the appropriate
target. The set of operations exposed is the **intersection** of what is
universally supported: ``subgroup_broadcast``, ``subgroup_ballot``,
``subgroup_any``, ``subgroup_all``, and the standard arithmetic reductions.
Operations that exist on only one or two backends — such as Metal's
``simd_shuffle_xor`` quad-shuffle — are not exposed.

The wave / SIMD-group **size** is queryable at runtime through
``GTEDeviceCapabilities::subgroupSize``, and a shader that depends on a
specific size must check this value at startup.

----

Variable Rate Shading
----------------------

Variable Rate Shading (VRS) lets the GPU shade pixels at a coarser rate in
regions that don't need full resolution.

**D3D12** supports VRS Tier 1 (per-draw) and Tier 2 (per-region image-based)
on most hardware from RDNA 2, Turing, and Intel Arc onward.

**Vulkan** supports VRS through ``VK_KHR_fragment_shading_rate``. Driver
support is uneven.

**Metal** does not currently expose a VRS equivalent. Apple silicon's tile-
based rendering achieves similar bandwidth savings through the tile memory
model, but there is no developer-facing API to control shading rate per
region.

Because Metal has no equivalent, OmegaGTE does not currently expose VRS
through the unified API. Applications that need it must use a backend-
specific code path.

----

Sparse Resources & Sampler Feedback
------------------------------------

**Sparse resources** (also called "tiled resources") let an application
allocate a logical buffer or texture that is much larger than physical GPU
memory and bind specific tiles on demand.

* **D3D12** supports tiled resources Tier 1 through Tier 3.
* **Vulkan** supports sparse binding through ``sparseBinding``,
  ``sparseResidency``, and related features.
* **Metal** does not support sparse resources. The feature is unavailable
  on the platform.

**Sampler feedback** lets the GPU report which mip levels and tiles a draw
call actually accessed, which is useful for streaming texture systems.

* **D3D12** supports sampler feedback through Shader Model 6.5.
* **Vulkan** does not have an equivalent.
* **Metal** does not have an equivalent.

Both features are reported through capability flags and are unavailable on
two of the three backends. Cross-platform applications should not depend on
either; engines that need them on Windows can branch on the capability flag
and fall back to a streaming heuristic on the other platforms.

----

Texture Compression Formats
----------------------------

Different platforms ship different hardware texture compression decoders.

* **Desktop GPUs** (used by D3D12 on Windows, Vulkan on Linux, Metal on
  Intel Macs) support the **BC** family: BC1 through BC7, including BC6H
  for HDR and BC7 for high-quality LDR.
* **Apple silicon** supports the BC family from M1 onward, plus **ASTC**
  (Adaptive Scalable Texture Compression).
* **Android / mobile Vulkan** typically supports **ETC2** and **ASTC** but
  not BC. Some Android devices add support for BC formats through extensions.

Textures shipped with an application must be in a format supported by the
target device. OmegaGTE does not transcode texture data on load. The
recommended pattern is to ship multiple compressed copies of each texture
asset and select the right format at load time based on the
``GTEDeviceCapabilities::supportedTextureFormats`` set.

----

Floating-Point Precision
-------------------------

* **Single-precision (32-bit)** float is universally supported.
* **Half-precision (16-bit)** float is universally supported in shaders and
  in vertex / pixel data formats. OmegaSL ``half`` types compile to ``half``
  on Metal, ``float16_t`` (with the ``KHR_shader_float16_int8`` extension)
  on Vulkan, and ``min16float`` or ``half`` on HLSL Shader Model 6.2+.
* **Double-precision (64-bit)** float is **not exposed** in OmegaSL.
  D3D12 supports it on hardware that exposes the relevant feature; Vulkan
  supports it through the ``shaderFloat64`` feature; Metal does not support
  it without per-pixel emulation. Including a feature whose performance and
  availability differ by an order of magnitude across backends would be
  misleading. Applications that genuinely need double-precision math should
  perform it on the CPU and pass the result to the GPU.

----

HDR Output
-----------

HDR display output requires both a hardware display capable of high
brightness and wide colour gamut and an OS-level signal to put the swap
chain into HDR mode.

**Direct3D 12** supports HDR through ``DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020``
swap chains on Windows 10 and later when the display reports HDR
capability.

**Metal** supports HDR through ``CAMetalLayer.wantsExtendedDynamicRangeContent``
on macOS and iOS displays that support EDR. The colour space and brightness
mapping are handled by the OS compositor.

**Vulkan** supports HDR on Linux through the
``VK_EXT_hdr_metadata`` extension, but the path from a Vulkan swap chain
to the display compositor is more variable than on Windows or macOS. Some
display servers (notably older versions of Xorg) cannot present HDR content
correctly.

OmegaGTE exposes a ``GERenderTargetDescriptor::hdr`` flag that is honored on
backends and displays where HDR is available, and is silently ignored on
displays that do not support it.

----

Multi-GPU
----------

**D3D12** has explicit multi-adapter support: an application can enumerate
each ``ID3D12Device`` and submit work to specific adapters using node masks.

**Vulkan** has device groups (``VkPhysicalDeviceGroupProperties``) on
hardware where the driver exposes them.

**Metal** does not have an explicit multi-GPU API. The ``MTLDevice`` for
external GPUs (eGPUs) on macOS Intel can be selected manually, but there is
no in-API mechanism for splitting work across two GPUs in a single render
pass.

Because the abstraction needed for true cross-GPU work distribution differs
fundamentally between D3D12 and Metal, OmegaGTE currently exposes only
single-device rendering. An application that needs multi-GPU behaviour on
Windows can construct multiple ``GTEDevice`` instances and submit
independent workloads to each, but cross-device synchronisation is not
provided.

----

The OmegaSL Language Surface
-----------------------------

OmegaSL is intentionally smaller than HLSL, MSL, or GLSL individually. It
exposes the operations that all three target languages can perform with the
same semantics. Features deliberately excluded:

* **Double-precision float** — see above.
* **Pointer arithmetic** — Metal allows it through ``device T*`` parameters;
  D3D12 does not in HLSL. Buffer access in OmegaSL is always indexed.
* **Recursion in shaders** — not supported on D3D12 or older Metal versions.
  All shader functions in OmegaSL are non-recursive.
* **Workgroup-shared dynamic allocation** — not supported across all
  backends. Threadgroup memory in OmegaSL is statically declared.
* **Texture-array layered writes from a fragment shader** — backend support
  is uneven. Fragment shaders write to a single render target slice.
* **Geometry shaders** — Metal does not implement them at all. OmegaGTE
  uses tessellation control + evaluation, plus optional mesh shaders, in
  their place.

These exclusions are intentional. A shader that uses only OmegaSL features
will compile and run identically on all three backends. A shader that needs
a feature outside this surface must be authored as a backend-specific
shader and gated behind a capability check.

----

Driver Quirks We Have Hit in Practice
--------------------------------------

In addition to formal feature differences, the three backends sometimes
disagree on edge cases that are not documented in any specification. The
following are issues OmegaGTE has had to design around or work around in
the engine itself.

**Multi-draw against a single root SRV on D3D12.**
When the Triangulation Engine emitted a complex shape (such as a rounded
rectangle) as nine separate ``TEMesh`` outputs — one per geometric piece —
the renderer issued nine consecutive ``DrawInstanced`` calls against a
single structured-buffer SRV bound at the root signature. On D3D12, only
the first one or two of those draws produced visible output even though
all nine were validated by the debug layer and reached the GPU with
correct vertex data. Metal and Vulkan rendered the same input correctly.
The fix was to **collapse all sub-meshes of a composite primitive into a
single mesh inside the engine** so the renderer issues a single
``DrawInstanced`` per primitive. As a result, the Triangulation Engine
guarantees one ``TEMesh`` per ``TETriangulationParams`` request. Composite
primitives are concatenated internally; consumers of ``TETriangulationResult``
see a single mesh per call.

**Triangle-strip topology and triangle-list vertex layout.**
A ``TEMesh`` whose vertex polygons are constructed as standalone triangles
must be emitted with ``TopologyTriangle``. Emitting the same data with
``TopologyTriangleStrip`` produced subtly wrong geometry on D3D12 in
particular, because the strip rasterizer reuses adjacent vertices in a way
the data did not intend. The Triangulation Engine now uses
``TopologyTriangle`` exclusively for primitives whose data is laid out as
independent triangles.

**Inconsistent winding inside a single primitive.**
Several legacy code paths in the Triangulation Engine produced one
clockwise triangle and one counter-clockwise triangle inside the same
mesh — for example, the two triangles of a rect. Even with culling
disabled, this inconsistency interacted poorly with D3D12 driver
optimisations that batch consecutive same-winding draws. The engine now
guarantees that every triangle of a primitive shares the winding requested
through ``GTEPolygonFrontFaceRotation``.

**Upload-heap buffer state transitions on D3D12.**
Buffers allocated from ``D3D12_HEAP_TYPE_UPLOAD`` must remain in the
``D3D12_RESOURCE_STATE_GENERIC_READ`` state for their entire lifetime.
Attempting to transition them — even to a state that the documentation
suggests is reachable — produces a debug-layer warning and undefined
behaviour. OmegaGTE's resource tracker detects upload-heap allocations
and skips state transitions on them automatically.

**MSL function constants vs. HLSL specialisation constants.**
Both languages have a notion of "compile-time-constant value injected at
pipeline build time", but the syntax and semantics differ enough that a
shared shader cannot reference a specialisation constant directly. OmegaSL
provides ``[[constant]]`` declarations that compile to function constants
on Metal, specialisation constants on Vulkan, and root constants on D3D12,
and that always present the same callable surface to the shader code.

**Driver version skew on Vulkan.**
The Vulkan extension landscape moves quickly, and some distributions ship
driver versions that pre-date the KHR ratification of an extension OmegaGTE
depends on. When this happens, the corresponding capability flag is
reported false at device creation and the application falls back to an
older path. There is no automatic driver-update mechanism in OmegaGTE; a
user running an out-of-date driver will see reduced functionality rather
than a runtime crash.

----

Practical Guidance
-------------------

When designing an application against OmegaGTE, the following habits avoid
running into platform limits late in development:

**Query capabilities before using optional features.**
Every optional feature listed above has a capability flag on
``GTEDeviceCapabilities``. Branch on the flag at startup and choose a
fallback path. Do not assume a feature is available because it works on
your development machine.

**Author shaders in OmegaSL unless you have a backend-specific reason not
to.**
A backend-specific shader is technical debt: it must be re-implemented on
every other platform or excluded from those platforms entirely. OmegaSL
shaders compile to all three targets from a single source file.

**Test on more than one backend.**
Driver behaviour, particularly around edge cases like degenerate triangles,
near-plane clipping, and resource state transitions, varies enough that an
application that works on one backend may render incorrectly on another.
The earlier in development this is caught, the cheaper it is to fix.

**Prefer fewer, larger draw calls to many small ones.**
The D3D12 multi-draw quirk above is the most concrete example, but the
general principle is that issuing one draw with one thousand triangles is
faster and more reliable than issuing one thousand draws with one triangle
each on every backend. The Triangulation Engine and other primitive
generators in OmegaGTE return a single mesh per request precisely so that
this advice can be followed by default.

**Treat the OmegaSL language surface as authoritative.**
If a feature is not in OmegaSL, it is either unavailable on at least one
backend or behaves differently enough across backends that exposing it
would create cross-platform bugs. The omission is the documentation.

----

Where the Limits Will Move
---------------------------

The set of features OmegaGTE exposes is not fixed. As the three backends
gain feature parity — for example, when Metal exposes a variable-rate
shading API, or when Vulkan adds a sampler-feedback equivalent — those
features become candidates for inclusion in the unified surface. The
intersection grows over time.

The reverse is also possible: a feature exposed today might be deprecated
in a future backend version, in which case OmegaGTE will retain a
compatibility path for as long as the backend itself does. The general
direction of travel is toward a larger shared surface, not a smaller one.

This document will be updated as the underlying backends evolve. The
authoritative source for what is and is not currently available on a
given device is the runtime ``GTEDeviceCapabilities`` query — this page
describes the design intent and the platform constraints that shape it.
