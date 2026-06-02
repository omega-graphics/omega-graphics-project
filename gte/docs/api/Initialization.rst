==============
Initialization
==============

Before you can allocate a single buffer or compile a single shader, OmegaGTE
has to be bound to a GPU. Initialization answers three questions: *which*
device, *what features* does it support, and *how loud* the debug layer
should be while you develop against it. This page walks through those three
decisions in the order you make them.

There is no umbrella ``OmegaGTE.h`` header — it was removed deliberately to
keep translation-unit compile times short. Include the specific headers you
need:

.. code-block:: cpp

    #include <omegaGTE/GTEDevice.h>   // GTE, GTEDevice, Init/Close, GTEInitOptions
    #include <omegaGTE/GE.h>          // OmegaGraphicsEngine, BufferDescriptor, …
    #include <omegaGTE/TE.h>          // OmegaTriangulationEngine, TETriangulationParams

.. contents:: On this page
   :local:
   :depth: 2

The `GTE` handle
================

After initialization you hold a single :cpp:struct:`OmegaGTE::GTE` value.
That struct is the root of the live engine state — every resource you
create, every pipeline you build, every triangulated mesh you produce comes
out of one of its three sub-engines:

.. cpp:struct:: OmegaGTE::GTE

    .. cpp:member:: SharedHandle<OmegaGraphicsEngine> graphicsEngine

        The graphics engine. Use this to make buffers, textures, samplers,
        heaps, pipelines, render targets, command queues, and acceleration
        structures. Everything in :doc:`Buffers`, :doc:`Textures`,
        :doc:`RenderPipeline`, etc. flows through here.

    .. cpp:member:: SharedHandle<OmegaTriangulationEngine> triangulationEngine

        The triangulation engine. Use this to turn high-level shapes
        (rectangles, ellipsoids, vector paths) into triangle meshes ready
        for the graphics engine. See :doc:`Triangulation`.

    .. cpp:member:: SharedHandle<OmegaSLCompiler> omegaSlCompiler

        Always-available runtime OmegaSL compiler. Lets you build a shader
        library from source strings instead of pre-compiled ``.omegasllib``
        archives. See :doc:`Shaders`.

The handle is cheap to copy — the underlying engines are reference-counted
via ``SharedHandle``. Treat it like any other owning value: keep it alive
for as long as the GPU side of your app needs to run, and pass it through
``Close()`` before the process exits so resources release in order.

Picking a device
================

OmegaGTE never *guesses* which GPU you want — you either ask the engine to
pick the system default, or you enumerate every device the OS exposes and
pick one yourself. Enumeration is the path you take whenever a discrete
GPU matters, or whenever the app needs to refuse to run on a device that
lacks a required feature (raytracing, mesh shaders, the higher MSAA tiers).

.. cpp:function:: OmegaCommon::Vector<SharedHandle<GTEDevice>> OmegaGTE::enumerateDevices()

    Returns every GPU visible to the OS, in the order the underlying API
    reports them.

A returned :cpp:struct:`OmegaGTE::GTEDevice` is read-only metadata about
one device — name, integrated/discrete type, the feature set, and a peek
at the native handle (``ID3D12Device *``, ``id<MTLDevice>``, or
``VkDevice``) for code that needs to drop down to the underlying API:

.. cpp:struct:: OmegaGTE::GTEDevice

    .. cpp:member:: const Type type

        ``GTEDevice::Integrated`` or ``GTEDevice::Discrete``. A laptop with
        both will report two entries from ``enumerateDevices()``.

    .. cpp:member:: const OmegaCommon::String name

        The driver-reported device name. Useful for displaying which GPU
        the app picked, and as a debugging signal if the wrong one was
        chosen.

    .. cpp:member:: const GTEDeviceFeatures features

        Capability snapshot. See *Inspecting device features* below.

    .. cpp:function:: const void *native()

        Returns the backend's native device handle, cast appropriately:
        ``ID3D12Device *`` on Windows, ``id<MTLDevice>`` on Apple, and
        ``VkDevice`` on Vulkan. Use this when you need to call into a
        backend-specific API that OmegaGTE doesn't wrap.

    .. cpp:function:: GTEDeviceMemoryBudget queryMemoryBudget()

        Snapshot of available VRAM (see *Memory budgets* below). Default
        returns zeros; supported backends override.

.. code-block:: cpp

    auto devices = OmegaGTE::enumerateDevices();

    // Prefer a discrete GPU; fall back to whatever's first.
    SharedHandle<OmegaGTE::GTEDevice> chosen = devices.front();
    for (auto & dev : devices) {
        if (dev->type == OmegaGTE::GTEDevice::Discrete) {
            chosen = dev;
            break;
        }
    }

    std::cout << "Using " << chosen->name << "\n";

Inspecting device features
==========================

Every device reports its feature set as a :cpp:struct:`OmegaGTE::GTEDeviceFeatures`
struct. Capabilities are packed into a 64-bit bitfield (``flags``) and
queried through :cpp:func:`hasFeature`; per-device numeric limits (texture
size, compute workgroup size, MSAA sample count) sit alongside as plain
fields. Always check the feature bit *before* you call into a code path
that depends on it — the engine's factory methods (acceleration structures,
mesh-shader pipelines) will return ``nullptr`` on unsupported devices
rather than throw.

.. cpp:struct:: OmegaGTE::GTEDeviceFeatures

    .. cpp:member:: uint64_t flags

        Bitmask of ``GTEDEVICE_FEATURE_*`` constants the device supports.

    .. cpp:member:: ShaderModel shaderModel

        Highest OmegaSL/HLSL shader model tier (``SM_5_0`` through
        ``SM_6_7``). Drives whether features like wave intrinsics, 16-bit
        types, and work graphs are usable.

    .. cpp:member:: uint8_t maxMSAASamples

        Highest MSAA sample count the device supports (1, 2, 4, 8, 16,
        or 32). Setting ``rasterSampleCount`` on a pipeline higher than
        this is rejected at pipeline creation.

    .. cpp:member:: uint32_t maxTextureDimension2D
    .. cpp:member:: uint32_t maxTextureDimension3D
    .. cpp:member:: uint32_t maxTextureDimensionCube

        Maximum side length of a single texture by kind.

    .. cpp:member:: uint64_t maxBufferSize

        Largest single ``GEBuffer`` allocation the device allows.

    .. cpp:member:: uint32_t maxComputeWorkGroupSizeX
    .. cpp:member:: uint32_t maxComputeWorkGroupSizeY
    .. cpp:member:: uint32_t maxComputeWorkGroupSizeZ
    .. cpp:member:: uint32_t maxComputeWorkGroupInvocations
    .. cpp:member:: uint32_t maxComputeSharedMemorySize

        Compute dispatch limits. ``Invocations`` is the cap on the *total*
        threads in one threadgroup (X×Y×Z); ``SharedMemorySize`` is per-
        threadgroup ``threadgroup<T>`` memory in bytes.

    .. cpp:member:: uint32_t maxSamplerAnisotropy

        Anisotropy ratio cap used by ``SamplerDescriptor::maxAnisotropy``.

    .. cpp:member:: float timestampPeriod

        Nanoseconds per GPU timestamp tick, or ``0.0f`` if the backend does
        not expose timestamp queries. Used by completion-callback timing
        (see :doc:`GPUSubmission`).

    .. cpp:function:: bool hasFeature(uint64_t featureMask) const

        Test that all bits in ``featureMask`` are set. Pass one or more
        ``GTEDEVICE_FEATURE_*`` constants OR'd together.

The full feature-flag set lives in ``GTEDevice.h``. The ones you are most
likely to test against:

.. list-table::
   :widths: 45 55
   :header-rows: 0

   * - ``GTEDEVICE_FEATURE_RAYTRACING``
     - Hardware BVH / ray-tracing pipelines (see :doc:`Raytracing`).
   * - ``GTEDEVICE_FEATURE_MESH_SHADER``
     - Mesh-shader pipelines.
   * - ``GTEDEVICE_FEATURE_VARIABLE_RATE_SHADING``
     - Variable-rate shading.
   * - ``GTEDEVICE_FEATURE_CONSERVATIVE_RASTERIZATION``
     - Conservative rasterisation.
   * - ``GTEDEVICE_FEATURE_INDEPENDENT_BLEND``
     - Per-attachment blend state.
   * - ``GTEDEVICE_FEATURE_DUAL_SOURCE_BLENDING``
     - Dual-source blending.
   * - ``GTEDEVICE_FEATURE_DEPTH_CLAMP``
     - Depth clamping in the rasteriser.
   * - ``GTEDEVICE_FEATURE_FILL_MODE_NON_SOLID``
     - Wireframe / point fill mode.
   * - ``GTEDEVICE_FEATURE_SAMPLER_ANISOTROPY``
     - Anisotropic sampler filter.
   * - ``GTEDEVICE_FEATURE_MULTI_DRAW_INDIRECT``
     - Multi-draw indirect (GPU-driven rendering).
   * - ``GTEDEVICE_FEATURE_GEOMETRY_SHADER``
     - Geometry shaders.
   * - ``GTEDEVICE_FEATURE_TESSELLATION_SHADER``
     - Tessellation shaders.
   * - ``GTEDEVICE_FEATURE_SHADER_BARYCENTRIC``
     - Barycentric coordinates in shaders.
   * - ``GTEDEVICE_FEATURE_DESCRIPTOR_INDEXING``
     - Bindless / dynamic descriptor indexing.
   * - ``GTEDEVICE_FEATURE_SHADER_FLOAT16``
     - 16-bit floats in shaders.
   * - ``GTEDEVICE_FEATURE_SHADER_INT16``
     - 16-bit ints in shaders.
   * - ``GTEDEVICE_FEATURE_SHADER_FLOAT64``
     - 64-bit doubles in shaders.
   * - ``GTEDEVICE_FEATURE_SHADER_INT64``
     - 64-bit ints in shaders.
   * - ``GTEDEVICE_FEATURE_TIMESTAMP_QUERIES``
     - GPU timestamp queries (for command-buffer timing).
   * - ``GTEDEVICE_FEATURE_TEXTURE_COMPRESSION_BC``
     - BC1–BC7 (Desktop).
   * - ``GTEDEVICE_FEATURE_TEXTURE_COMPRESSION_ETC2``
     - ETC2 (Mobile).
   * - ``GTEDEVICE_FEATURE_TEXTURE_COMPRESSION_ASTC``
     - ASTC (Mobile, some Desktop).

Plus ``GTEDEVICE_FEATURE_WIDE_LINES``, ``GTEDEVICE_FEATURE_DEPTH_BIAS_CLAMP``,
``GTEDEVICE_FEATURE_DRAW_INDIRECT_FIRST_INSTANCE`` for niche cases.

.. code-block:: cpp

    auto & f = chosen->features;

    if (!f.hasFeature(OmegaGTE::GTEDEVICE_FEATURE_RAYTRACING)) {
        std::cerr << "This device cannot run our raytracing path.\n";
        // …fall back to rasterisation…
    }

    // A pipeline that needs both mesh shaders AND barycentrics.
    constexpr uint64_t MESH_AND_BARY =
        OmegaGTE::GTEDEVICE_FEATURE_MESH_SHADER |
        OmegaGTE::GTEDEVICE_FEATURE_SHADER_BARYCENTRIC;
    if (f.hasFeature(MESH_AND_BARY)) {
        // …use the mesh-shader path…
    }

    if (f.shaderModel < OmegaGTE::GTEDeviceFeatures::ShaderModel::SM_6_0) {
        // No wave intrinsics — pick the legacy compute kernel.
    }

Memory budgets
==============

On dedicated-VRAM systems (most discrete GPUs) you can ask the device for
its current memory budget snapshot. On unified-memory systems (Apple
Silicon, integrated parts) the dedicated-VRAM number is zero — the device
reports the unified flag and you should consult system memory queries for
budgeting instead.

.. cpp:struct:: OmegaGTE::GTEDeviceMemoryBudget

    .. cpp:member:: uint64_t dedicatedVideoMemory

        Total VRAM physically attached to the device, in bytes. Zero on
        unified-memory architectures.

    .. cpp:member:: uint64_t availableVideoMemory

        How many of those bytes are currently free for OmegaGTE to allocate
        into. Read this between frames if you stream large assets.

    .. cpp:member:: bool unifiedMemory

        ``true`` when the device shares RAM with the CPU. When this is
        true the other two fields are meaningful only for the OS's reserved
        portion, if at all.

Configuring the debug layer
===========================

OmegaGTE wraps each backend's validation layer behind one option struct.
You pass it to :cpp:func:`Init` (or :cpp:func:`InitWithDefaultDevice`) and
it is frozen for the process lifetime — there is no runtime toggle. The
debug layer is what makes the difference between a silent black screen and
a console line that tells you exactly which descriptor binding mismatched
which slot.

.. cpp:struct:: OmegaGTE::GTEInitOptions

    .. cpp:member:: DebugLayer debugLayer

        Tri-state toggle:

        * ``Default`` — follow the ``OMEGAGTE_DEBUG`` compile flag (on in
          Debug builds, off in Release).
        * ``Enabled`` — force the backend validation layer on plus verbose
          ``DEBUG_STREAM`` logging. Use this when chasing a release-only
          bug from a Debug binary.
        * ``Disabled`` — force off. Use when you need a Debug-built binary
          to measure something close to release-grade performance.

    .. cpp:member:: bool gpuBasedValidation

        Enable D3D12 GBV / Vulkan GPU-assisted validation. **Expensive** —
        each draw runs 5–10× slower because every binding is checked on
        the GPU. Use to chase resource-state bugs that the CPU-side
        validation cannot see. Ignored if ``debugLayer`` resolves off.

    .. cpp:member:: bool captureOnInit

        **Metal only.** Start a programmatic GPU frame capture at
        ``Init()`` and stop it at ``Close()``, writing a ``.gputrace``
        document. No-op on D3D12 and Vulkan; silently skipped if the
        embedding app does not set ``MetalCaptureEnabled=YES`` in its
        ``Info.plist`` or the ``MTL_CAPTURE_ENABLED=1`` env var. Gated
        behind its own flag (not just ``debugLayer``) because traces grow
        fast.

    .. cpp:member:: const char *captureFilePath

        Where to write the Metal capture document. ``nullptr`` or empty
        means "use the default ``omegagte-<pid>-<timestamp>.gputrace`` in
        the working directory".

These free functions let you check the resolved state after ``Init()``:

.. cpp:function:: bool OmegaGTE::isDebugLayerEnabled()

    Whether the debug layer ended up on after merging the option with the
    compile-time default.

.. cpp:function:: bool OmegaGTE::isGpuBasedValidationEnabled()

    Whether GBV was requested. Always false if the debug layer is off.

.. cpp:function:: bool OmegaGTE::isCaptureOnInitEnabled()

    Metal capture status. Always false if the debug layer is off.

.. cpp:function:: const char *OmegaGTE::captureOutputPath()

    The path the capture is being written to. Never ``nullptr``; an empty
    string means "default file in the working directory".

Starting and stopping the engine
================================

With a device chosen and options set, you start the engine and get the
:cpp:struct:`GTE` handle. Two entry points: one that takes a specific
device, one that lets OmegaGTE pick:

.. cpp:function:: GTE OmegaGTE::Init(SharedHandle<GTEDevice> & device, GTEInitOptions opts = {})

    Initialise for a specific device. Returns the live ``GTE`` handle.

.. cpp:function:: GTE OmegaGTE::InitWithDefaultDevice(GTEInitOptions opts = {})

    Initialise using the system's default (best available) device. Skip
    the ``enumerateDevices()`` step when the app does not need feature
    gating.

.. cpp:function:: void OmegaGTE::Close(GTE & gte)

    Shut both engines down and release the GPU resources they hold. Call
    this before the process exits, after every render target and command
    queue has been destroyed or has finished its in-flight work. Calling
    ``Close`` while GPU work is outstanding leaves the device in a
    half-released state on some backends.

.. code-block:: cpp

    // Pick a device manually and turn the debug layer on hard.
    auto devices = OmegaGTE::enumerateDevices();
    auto device  = devices[0];

    OmegaGTE::GTEInitOptions opts;
    opts.debugLayer         = OmegaGTE::GTEInitOptions::DebugLayer::Enabled;
    opts.gpuBasedValidation = true;   // catch resource-state bugs

    OmegaGTE::GTE gte = OmegaGTE::Init(device, opts);

    if (OmegaGTE::isDebugLayerEnabled())
        std::cout << "Debug layer ON; capture path = "
                  << OmegaGTE::captureOutputPath() << "\n";

    // …drive the engine through gte.graphicsEngine / .triangulationEngine…

    OmegaGTE::Close(gte);

For most apps the short form is enough — no enumeration, default options:

.. code-block:: cpp

    OmegaGTE::GTE gte = OmegaGTE::InitWithDefaultDevice();
    // …work…
    OmegaGTE::Close(gte);

Common pitfalls
===============

* **Calling** ``Close()`` **with in-flight GPU work.** Wait on each command
  queue (``commitToGPUAndWait``) or each render target (``waitForGPU()``)
  before tearing down. See :doc:`GPUSubmission`.
* **Treating a missing feature bit as a runtime error.** It is a startup
  decision: check ``hasFeature`` once after device selection and pick the
  code path then, rather than scattering checks through the draw loop.
* **Forgetting that ``maxMSAASamples`` is a hard cap.** Pipelines with a
  higher ``rasterSampleCount`` than the device supports fail to create.
  Read ``features.maxMSAASamples`` before picking your sample count.
* **Expecting** ``captureOnInit`` **to work without Info.plist opt-in.**
  Apple requires the embedding app to declare capture support; OmegaGTE
  cannot bypass that. Without it, capture silently skips.
* **Holding** ``GTEDevice::native()`` **past** ``Close()``. The underlying
  device is released as part of shutdown — the native pointer dangles.
