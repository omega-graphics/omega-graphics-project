==========
Raytracing
==========

Ray tracing in OmegaGTE uses *acceleration structures* — BVH trees built by
the GPU — so a compute shader can ask "what does this ray hit, given this
scene" without walking the geometry itself. The pattern is always the
same: describe the scene's geometry in a
:cpp:struct:`OmegaGTE::GEAccelerationStructDescriptor`, allocate an empty
:cpp:struct:`OmegaGTE::GEAccelerationStruct`, build it on the GPU inside
an acceleration-structure pass, then bind it into a raytracing compute
pipeline and dispatch rays.

The whole API is **runtime-gated**, not compile-time gated. The factory
methods and the ``dispatchRays`` entry point are always present in the
build — they are present in the headers regardless of the platform. On a
device that does not advertise ``GTEDEVICE_FEATURE_RAYTRACING`` the
factories return ``nullptr`` rather than throwing or crashing, so your
runtime fallback path is straightforward to write.

.. contents:: On this page
   :local:
   :depth: 2

Feature gating
==============

Always check device support before using any of this:

.. code-block:: cpp

    if (!device->features.hasFeature(OmegaGTE::GTEDEVICE_FEATURE_RAYTRACING)) {
        // Pick a rasterised fallback path instead.
        return;
    }

A shader that uses raytracing intrinsics also has to declare it requires
the ``raytracing`` feature in OmegaSL. The library loader rejects the
shader (and marks it as an unsupported sentinel — see :doc:`Shaders`) on
devices without the feature bit. Building a compute pipeline against a
sentinel returns ``nullptr``.

Acceleration structures
=======================

Acceleration structures package geometry the GPU can intersect efficiently.
The descriptor names the source data; the structure itself is opaque.

.. cpp:struct:: OmegaGTE::GEAccelerationStructDescriptor

    .. cpp:function:: void addTriangleBuffer(SharedHandle<GEBuffer> & buffer)

        Add a triangle list to the structure. The buffer's contents are
        an array of vertex positions; the build path indexes them as
        consecutive triangles.

    .. cpp:function:: void addBoundingBoxBuffer(SharedHandle<GEBuffer> & buffer)

        Add a buffer of axis-aligned bounding boxes for *procedural*
        geometry — the GPU intersects against the AABBs, and your
        compute kernel does the inner intersection test against the
        actual primitive. Build the AABB buffer via
        :cpp:func:`OmegaGraphicsEngine::createBoundingBoxesBuffer` from an
        ``ArrayRef<GERaytracingBoundingBox>``.

    Internally, each entry becomes a :cpp:struct:`Geometry` carrying either
    a :cpp:struct:`TriangleList` or an :cpp:struct:`Aabb` payload in a
    ``std::variant``. Most code does not need to touch the variant
    directly — use the ``add*`` helpers — but the lower-level access is
    useful when iterating over the contents of a descriptor.

.. cpp:struct:: OmegaGTE::GEAccelerationStructDescriptor::Geometry

    .. cpp:type:: TriangleList

        Wraps one ``SharedHandle<GEBuffer>`` of vertex data.

    .. cpp:type:: Aabb

        Wraps one ``SharedHandle<GEBuffer>`` of
        :cpp:struct:`GERaytracingBoundingBox` AABBs.

    .. cpp:function:: void setTriangleList(SharedHandle<GEBuffer> & buffer)
    .. cpp:function:: void setAabb(SharedHandle<GEBuffer> & buffer)
    .. cpp:function:: TriangleList & getTriangleList()
    .. cpp:function:: Aabb & getAabb()

.. cpp:struct:: OmegaGTE::GERaytracingBoundingBox

    A single AABB for procedural geometry. Pack an array of these into a
    buffer with
    :cpp:func:`OmegaGraphicsEngine::createBoundingBoxesBuffer`, then pass
    that buffer to :cpp:func:`addBoundingBoxBuffer`.

    .. cpp:member:: float minX
    .. cpp:member:: float minY
    .. cpp:member:: float minZ
    .. cpp:member:: float maxX
    .. cpp:member:: float maxY
    .. cpp:member:: float maxZ

.. cpp:struct:: OmegaGTE::GEAccelerationStruct

    Opaque GPU acceleration structure. Bind to a compute shader at an
    OmegaSL register with
    :cpp:func:`bindResourceAtComputeShader`. See :doc:`ComputePipeline`.

Building, copying, and refitting
================================

Acceleration-structure work is recorded in its own pass type on a command
buffer (see :doc:`GPUSubmission`). The build is GPU-side — submit it like
any other pass and wait on completion before binding the result.

.. cpp:function:: void GECommandBuffer::beginAccelStructPass()
.. cpp:function:: void GECommandBuffer::finishAccelStructPass()

    Open and close the acceleration-structure pass.

.. cpp:function:: void GECommandBuffer::buildAccelerationStructure(\
    SharedHandle<GEAccelerationStruct> & src,\
    const GEAccelerationStructDescriptor & desc)

    Build ``src`` from the descriptor. Pair an
    :cpp:func:`allocateAccelerationStructure` allocation with one build
    pass before binding.

.. cpp:function:: void GECommandBuffer::copyAccelerationStructure(\
    SharedHandle<GEAccelerationStruct> & src,\
    SharedHandle<GEAccelerationStruct> & dest)

    Copy a compact / cloned acceleration structure into ``dest``. Use for
    transferring a built structure between caches or pipelines.

.. cpp:function:: void GECommandBuffer::refitAccelerationStructure(\
    SharedHandle<GEAccelerationStruct> & src,\
    SharedHandle<GEAccelerationStruct> & dest,\
    const GEAccelerationStructDescriptor & desc)

    Refit a previously-built structure with updated vertex data. Cheaper
    than a full rebuild when only positions change (animated meshes that
    keep the same topology).

End-to-end example
==================

.. code-block:: cpp

    if (!device->features.hasFeature(OmegaGTE::GTEDEVICE_FEATURE_RAYTRACING))
        return;

    // 1. Describe the geometry.
    OmegaGTE::GEAccelerationStructDescriptor asDesc;
    asDesc.addTriangleBuffer(vertexBuffer);

    // 2. Allocate the empty structure.
    auto accelStruct = gte.graphicsEngine->allocateAccelerationStructure(asDesc);
    if (!accelStruct) {
        std::cerr << "Allocation failed (device lacks raytracing?).\n";
        return;
    }

    // 3. Build it on the GPU.
    auto buildCmd = queue->getAvailableBuffer();
    buildCmd->beginAccelStructPass();
    buildCmd->buildAccelerationStructure(accelStruct, asDesc);
    buildCmd->finishAccelStructPass();
    queue->submitCommandBuffer(buildCmd);
    queue->commitToGPUAndWait();   // wait — bind below assumes it is done

    // 4. Dispatch rays via a raytracing compute pipeline.
    auto rtCmd = queue->getAvailableBuffer();
    OmegaGTE::GEComputePassDescriptor passDesc{};
    rtCmd->startComputePass(passDesc);
    rtCmd->setComputePipelineState(raytracingPipeline);
    rtCmd->bindResourceAtComputeShader(accelStruct, 0);
    rtCmd->bindResourceAtComputeShader(outputImage,  1);
    rtCmd->dispatchRays(width, height, 1);
    rtCmd->finishComputePass();
    queue->submitCommandBuffer(rtCmd);
    queue->commitToGPU();

For procedural geometry (AABBs instead of triangles):

.. code-block:: cpp

    std::vector<OmegaGTE::GERaytracingBoundingBox> spheres = {
        { -1, -1, -1, 1, 1, 1 },
        {  2, -1, -1, 4, 1, 1 },
    };
    auto aabbBuffer = gte.graphicsEngine->createBoundingBoxesBuffer(spheres);

    OmegaGTE::GEAccelerationStructDescriptor asDesc;
    asDesc.addBoundingBoxBuffer(aabbBuffer);

    auto accelStruct = gte.graphicsEngine->allocateAccelerationStructure(asDesc);
    // …same build + dispatchRays steps as above…

Common pitfalls
===============

* **Skipping the feature check.** ``allocateAccelerationStructure`` and
  ``createBoundingBoxesBuffer`` return ``nullptr`` on devices without the
  raytracing bit. Treat null as "no raytracing here" and pick a
  rasterised path.
* **Dispatching rays before the build pass finishes.** The build is GPU
  work — synchronise with a fence or ``commitToGPUAndWait`` before
  submitting the consumer.
* **Refitting after topology changes.** ``refitAccelerationStructure`` is
  safe only when index / triangle counts and the structure shape are
  unchanged. Rebuilt geometry needs a full build.
* **Mismatched bindings.** A raytracing compute shader expects the
  acceleration structure at the OmegaSL register it declares; verify the
  ``id`` you pass to ``bindResourceAtComputeShader`` matches.
* **Using a sentinel shader.** If
  :cpp:func:`OmegaGraphicsEngine::makeComputePipelineState` returns
  ``nullptr``, read ``computeFunc->isUnsupported`` and
  ``unsupportedDiagnostic`` (see :doc:`Shaders`) for the precise
  device-feature reason.
