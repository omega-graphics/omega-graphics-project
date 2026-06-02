================
Compute Pipeline
================

A compute pipeline wraps a single OmegaSL compute shader entry point. Once
built, you bind it inside a *compute pass* on a command buffer, attach the
buffers and textures it needs, and dispatch one or more threadgroups. Use
compute for everything that is not "rasterise these primitives": physics,
particle updates, post-process effects, image filtering, raytracing
(see :doc:`Raytracing`), and any GPU-side data-parallel work.

Compute passes are encoded on the same :cpp:class:`OmegaGTE::GECommandBuffer`
that renders frames. There is no separate "compute queue" type — submit
compute work onto a regular :doc:`GECommandQueue <GPUSubmission>`. On
backends with async compute (D3D12, Vulkan), making a dedicated queue lets
the driver overlap compute with rendering.

.. contents:: On this page
   :local:
   :depth: 2

ComputePipelineDescriptor
=========================

.. cpp:struct:: OmegaGTE::ComputePipelineDescriptor

    .. cpp:member:: OmegaCommon::String name

        Debug label.

    .. cpp:member:: SharedHandle<GTEShader> computeFunc

        Compute shader entry point from a loaded
        :cpp:struct:`GTEShaderLibrary`. See :doc:`Shaders`.

.. code-block:: cpp

    OmegaGTE::ComputePipelineDescriptor desc;
    desc.name        = "ParticleUpdate";
    desc.computeFunc = shaderLib->shaders["updateParticles"];
    auto computePipeline = gte.graphicsEngine->makeComputePipelineState(desc);

The pipeline carries enough information to dispatch — including the
threadgroup dimensions declared in the OmegaSL ``compute(x=..., y=...,
z=...)`` attribute — so the dispatch call only needs to specify *how many
groups* (or threads) to run.

Threadgroups vs. threads
========================

OmegaGTE exposes two dispatch entry points:

* :cpp:func:`dispatchThreadgroups` takes a **group count** — three integers
  whose product is the number of threadgroups to launch. Each group is
  whatever size the shader declared in its ``compute(x=N, y=M, z=P)``
  attribute. Use this when you know the group count directly — most
  hand-tuned kernels.
* :cpp:func:`dispatchThreads` takes a **total thread count**. The backend
  divides by the threadgroup size internally and dispatches the right
  number of groups. Use this when it is easier to think about "total
  threads" than "groups × group size", or when the workload happens not to
  be a clean multiple of the group dimensions and you would otherwise
  round up by hand.

Both forms are functionally equivalent; the second saves you one ``ceil``
per dispatch. Either way, ``dispatchThreadgroups(width/8, height/8, 1)``
and ``dispatchThreads(width, height, 1)`` produce identical work for a
shader declared with ``compute(x=8, y=8, z=1)``.

Compute pass encoding
=====================

All of these live on :cpp:class:`OmegaGTE::GECommandBuffer`.

.. cpp:function:: void GECommandBuffer::startComputePass(const GEComputePassDescriptor & desc)

    Open a compute pass. The descriptor is currently empty — pass a
    default-constructed ``GEComputePassDescriptor{}``.

.. cpp:function:: void GECommandBuffer::setComputePipelineState(SharedHandle<GEComputePipelineState> & pipelineState)

    Bind the active compute pipeline.

.. cpp:function:: void GECommandBuffer::bindResourceAtComputeShader(SharedHandle<GEBuffer> & buffer, unsigned id)
.. cpp:function:: void GECommandBuffer::bindResourceAtComputeShader(SharedHandle<GETexture> & texture, unsigned id, const TextureSwizzle & swizzle = TextureSwizzle::identity())
.. cpp:function:: void GECommandBuffer::bindResourceAtComputeShader(SharedHandle<GESamplerState> & sampler, unsigned id)
.. cpp:function:: void GECommandBuffer::bindResourceAtComputeShader(SharedHandle<GEAccelerationStruct> & accelStruct, unsigned id)

    Bind a buffer, texture (with optional per-bind swizzle), runtime
    sampler, or raytracing acceleration structure to the compute shader at
    OmegaSL register ``id``. Acceleration-structure binds are used by
    raytracing kernels (see :doc:`Raytracing`).

.. cpp:function:: void GECommandBuffer::setComputeConstants(const void *data, unsigned size, unsigned offset = 0)

    Update the bound pipeline's push-constant block (OmegaSL
    ``constant<T>``). Compute-pass mirror of
    :cpp:func:`setRenderConstants` — ≤ 128 bytes portable, no slot
    argument, no packing, Metal supports only ``offset == 0``. Must be
    called inside a compute pass with a pipeline bound.

.. cpp:function:: void GECommandBuffer::dispatchThreadgroups(unsigned x, unsigned y, unsigned z)

    Dispatch ``x * y * z`` thread groups. Each group runs with the
    dimensions declared in the OmegaSL ``compute(x=N, y=M, z=P)``.

.. cpp:function:: void GECommandBuffer::dispatchThreads(unsigned x, unsigned y, unsigned z)

    Dispatch exactly ``x * y * z`` *threads*. The backend computes the
    matching threadgroup count internally.

.. cpp:function:: void GECommandBuffer::dispatchThreadgroupsIndirect(SharedHandle<GEBuffer> & argumentBuffer, size_t argumentBufferOffset)

    GPU-driven dispatch — read the group count from a buffer the GPU
    wrote. The buffer must hold a :cpp:struct:`GEDispatchIndirectCommand`
    at ``argumentBufferOffset``.

.. cpp:function:: void GECommandBuffer::dispatchRays(unsigned x, unsigned y, unsigned z)

    Ray-tracing dispatch. Bind a raytracing pipeline (a compute pipeline
    whose shader uses raytracing intrinsics) plus the acceleration
    structures it needs, then call this. ``x``, ``y``, ``z`` are the rays
    per axis. See :doc:`Raytracing`.

.. cpp:function:: void GECommandBuffer::finishComputePass()

    Close the compute pass.

.. cpp:struct:: OmegaGTE::GEDispatchIndirectCommand

    Argument layout for :cpp:func:`dispatchThreadgroupsIndirect` — matches
    the underlying D3D12 / Vulkan / Metal struct byte-for-byte.

    .. cpp:member:: std::uint32_t groupCountX
    .. cpp:member:: std::uint32_t groupCountY
    .. cpp:member:: std::uint32_t groupCountZ

Complete dispatch examples
==========================

.. code-block:: cpp

    auto cmdBuf = queue->getAvailableBuffer();

    OmegaGTE::GEComputePassDescriptor passDesc{};
    cmdBuf->startComputePass(passDesc);
    cmdBuf->setComputePipelineState(computePipeline);
    cmdBuf->bindResourceAtComputeShader(inputBuffer,  0);
    cmdBuf->bindResourceAtComputeShader(outputBuffer, 1);

    // Shader declared compute(x=64, y=1, z=1); 1024 elements → 16 groups.
    cmdBuf->dispatchThreadgroups(16, 1, 1);

    cmdBuf->finishComputePass();
    queue->submitCommandBuffer(cmdBuf);
    queue->commitToGPUAndWait();
    cmdBuf->reset();

The thread-count variant for the same kernel:

.. code-block:: cpp

    cmdBuf->startComputePass(passDesc);
    cmdBuf->setComputePipelineState(computePipeline);
    cmdBuf->bindResourceAtComputeShader(inputBuffer,  0);
    cmdBuf->bindResourceAtComputeShader(outputBuffer, 1);
    cmdBuf->dispatchThreads(1024, 1, 1);    // backend rounds to 16 groups
    cmdBuf->finishComputePass();

A 2D image filter (512 × 512, ``compute(x=8, y=8, z=1)`` → 64 × 64 groups)
with per-frame constants pushed via the constant API:

.. code-block:: cpp

    cmdBuf->startComputePass(passDesc);
    cmdBuf->setComputePipelineState(blurPipeline);
    cmdBuf->bindResourceAtComputeShader(inputImage,   0);
    cmdBuf->bindResourceAtComputeShader(outputImage,  1);
    cmdBuf->bindResourceAtComputeShader(linearSampler,2);

    struct BlurConstants { float radius; float sigma; uint32_t pass; };
    BlurConstants kc{ 4.0f, 1.5f, 0 };
    cmdBuf->setComputeConstants(&kc, sizeof(kc));

    cmdBuf->dispatchThreadgroups(512 / 8, 512 / 8, 1);
    cmdBuf->finishComputePass();

An indirect dispatch driven by a previous compute pass that wrote group
counts into a buffer:

.. code-block:: cpp

    // Earlier in the frame, an "analyse" pass wrote one GEDispatchIndirectCommand
    // into `dispatchArgs` at offset 0.
    cmdBuf->startComputePass(passDesc);
    cmdBuf->setComputePipelineState(processPipeline);
    cmdBuf->bindResourceAtComputeShader(workBuffer, 0);
    cmdBuf->dispatchThreadgroupsIndirect(dispatchArgs, 0);
    cmdBuf->finishComputePass();

Common pitfalls
===============

* **Confusing thread count and group count.** ``dispatchThreadgroups(1024,
  1, 1)`` with a shader declared at ``compute(x=64, y=1, z=1)`` launches
  *65536* threads, not 1024. Reach for ``dispatchThreads`` when the
  workload is sized in elements.
* **Workgroup size larger than device limits.** Check
  :cpp:member:`GTEDeviceFeatures::maxComputeWorkGroupInvocations` (total
  threads per group) and the per-axis maxima before settling on a
  ``compute(x=N, y=M, z=P)`` shape.
* **Forgetting** ``finishComputePass`` **before submission.** Submitting a
  command buffer with an open pass tears the validation layer down
  loudly. Always pair ``startComputePass`` with ``finishComputePass``.
* **Push constants without a bound pipeline.** ``setComputeConstants`` must
  follow ``setComputePipelineState`` and live inside a compute pass.
* **Reading the output buffer before the GPU finishes.** A compute pass
  enqueues work; it does not wait. Use ``commitToGPUAndWait``, a fence,
  or a completion callback before reading results (see
  :doc:`GPUSubmission` and :doc:`Buffers`).
* **Indirect arguments with the wrong shape.** The buffer must hold a
  :cpp:struct:`GEDispatchIndirectCommand` at ``argumentBufferOffset``;
  any other layout dispatches the wrong number of groups.
