==============
GPU Submission
==============

Drawing, dispatching, and blitting are all encoded into a **command buffer**
and then submitted to the GPU through a **command queue**. Synchronisation
across queues, across the GPU and CPU, and between producing and consuming
work is done with **fences**. Completion callbacks let the CPU observe
when a buffer has finished executing — useful for profiling, asynchronous
readback, and lifetime management.

This page describes the plumbing that everything in :doc:`RenderPipeline`,
:doc:`ComputePipeline`, and :doc:`Blitting` ultimately submits through.

.. contents:: On this page
   :local:
   :depth: 2

The mental model
================

OmegaGTE keeps the model deliberately small:

* A **command queue** is a fixed-size pool of command buffers backed by a
  GPU execution stream. Buffers submitted to one queue execute *in order*
  on the GPU; buffers on different queues can run concurrently.
* A **command buffer** is a recording of a sequence of passes (render,
  compute, blit, acceleration-structure). It is opaque between recording
  and submission — the queue plays it back on the GPU.
* A **fence** is a 64-bit counter the GPU advances. One queue *signals* a
  fence, another queue *waits* on it. The CPU can also signal or wait on
  the same fence — fences are the cross-queue and cross-side
  synchronisation primitive.

Pick the queue count to match the kinds of work you want to overlap: one
"main" queue is enough for most apps; add an "async compute" queue when a
compute kernel can run in parallel with rendering; add a "copy" queue for
streaming uploads that should not contend with frame work.

Command queues
==============

.. cpp:class:: OmegaGTE::GECommandQueue

    A growable pool of command buffers, plus a GPU execution slot. Built
    with ``OmegaGraphicsEngine::makeCommandQueue(maxBufferCount)`` (or
    the typed-pool overload ``makeCommandQueue(const GECommandQueueDesc &)``).
    The buffer count you pass is the **initial pool size hint**, not a
    hard cap — the queue allocates more on demand if every slot is still
    in flight, up to a backend-defined ceiling of 256.

    .. cpp:function:: SharedHandle<GECommandBuffer> getAvailableBuffer()

        Return a command buffer that the GPU is no longer using. The
        queue tracks each slot's last submission via its retention
        fence / timeline semaphore: it recycles a slot once the GPU
        has caught up. If every slot is still in flight, the pool grows
        by one (up to the 256 ceiling); the queue logs a one-shot warning
        the first time the pool exceeds 4× the initial hint, so a
        consistently under-sized hint surfaces in the build log but
        never floods it. At the ceiling the call returns ``nullptr`` —
        check for that if you are pushing thousands of buffers without
        committing.

    .. cpp:function:: unsigned getSize()

        Total number of command buffers in the pool.

    .. cpp:function:: void submitCommandBuffer(SharedHandle<GECommandBuffer> & commandBuffer)

        Enqueue the buffer for execution. The buffer runs after any
        previously enqueued buffer on this queue.

    .. cpp:function:: void submitCommandBuffer(SharedHandle<GECommandBuffer> & commandBuffer, SharedHandle<GEFence> & signalFence)

        Enqueue and signal ``signalFence`` when the buffer completes on
        the GPU. The fence's counter increments by 1.

    .. cpp:function:: void notifyCommandBuffer(SharedHandle<GECommandBuffer> & commandBuffer, SharedHandle<GEFence> & waitFence)

        Encode a fence wait at the *beginning* of the buffer so it stalls
        on the GPU until ``waitFence`` reaches the value last signalled
        by the producer queue. Pair with the signalling submit on the
        other queue.

    .. cpp:function:: void commitToGPU()

        Submit every enqueued buffer to the GPU. Does not wait.

    .. cpp:function:: void commitToGPUAndWait()

        Submit every enqueued buffer and block on the CPU until the GPU
        finishes the last one. The cheap synchronisation primitive when
        you are not yet using fences.

    .. cpp:function:: void signalFence(SharedHandle<GEFence> & fence)

        Signal a fence on this queue without a command buffer. Useful as
        a "I am done, downstream queues may proceed" marker after
        ``waitForGPU()``.

    .. cpp:function:: void waitForFence(SharedHandle<GEFence> & fence, std::uint64_t value)

        CPU-side wait — block until ``fence`` reaches at least ``value``.
        Use to coordinate CPU work that needs to read GPU results.

Queue creation: ``GECommandQueueDesc``
--------------------------------------

Every queue is built from a ``GECommandQueueDesc``. The descriptor names
what the queue is for — its logical type, its execution priority, the
initial buffer-pool size, and an optional debug label — so the engine
can pick the right Vulkan queue family / D3D12 command-list type at
creation time instead of collapsing every queue onto the universal
family.

.. cpp:enum-class:: GECommandQueueDesc::Type

    * ``Universal`` — render + compute + copy. The default; corresponds
      to a D3D12 DIRECT queue, a Vulkan family that exposes graphics +
      compute + transfer, and the lone ``MTLCommandQueue`` family on
      Metal.
    * ``Graphics`` — render + compute. Same family as ``Universal`` on
      most devices; on Vulkan picks a graphics-capable family
      (preferring one that also has compute) so the queue can compute-
      prepass without crossing a family boundary.
    * ``Compute`` — async compute. Vulkan picks a compute-only family
      when the device exposes one (typical on NVIDIA Pascal+, AMD GCN,
      Intel Xe-HPG), falling back to the graphics family otherwise.
      D3D12 always returns a COMPUTE queue. Metal records the type for
      introspection only.
    * ``Transfer`` — DMA / dedicated-copy. Vulkan picks a
      transfer-only family when the device exposes one (the discrete
      PCIe copy engine on most discrete GPUs), falling back to compute
      and then to graphics. D3D12 always returns a COPY queue. A
      Transfer queue cannot be used as the present queue for
      ``makeNativeRenderTarget`` — that call fails at descriptor parse
      time.

.. cpp:enum-class:: GECommandQueueDesc::Priority

    * ``Low`` / ``Normal`` / ``High`` / ``Realtime`` — Vulkan honors the
      first three via relative-float ``pQueuePriorities[]`` and chains
      ``VK_KHR_global_priority MEDIUM`` per opened family when the
      extension is available; HIGH/LOW/NORMAL Universal requests on a
      desktop GPU resolve to distinct ``VkQueue`` handles. ``Realtime``
      falls back to ``High`` because the global-priority entitlement
      (``CAP_SYS_NICE`` on Linux, the GPU-priority entitlement on
      Windows) is commonly denied. D3D12 maps to
      ``D3D12_COMMAND_QUEUE_PRIORITY_*`` and silently downgrades
      Realtime to High when the entitlement is missing. Metal has no
      public priority API; the value is recorded for introspection only.

The remaining fields:

* ``maxBufferCount`` — initial pool-size hint (see
  ``getAvailableBuffer`` above for the growable-pool semantics).
* ``requireDedicated`` — if true, the engine refuses to fall back to a
  more-general queue family. Use in tests that assert a dedicated async
  compute or transfer family is actually present.
* ``label`` — optional debug name. Plumbed to
  ``ID3D12Object::SetName`` (D3D12), ``VK_EXT_debug_utils``
  (Vulkan), and ``[MTLCommandQueue setLabel:]`` (Metal).

.. code-block:: cpp

    // Pool of three command buffers — enough for double-buffering plus
    // a spare. Default type is Universal, default priority is Normal.
    OmegaGTE::GECommandQueueDesc desc{};
    desc.maxBufferCount = 3;
    auto queue = gte.graphicsEngine->makeCommandQueue(desc);

    auto cmd = queue->getAvailableBuffer();
    cmd->setName("Frame");
    // …encode passes…
    queue->submitCommandBuffer(cmd);
    queue->commitToGPUAndWait();
    cmd->reset();

    // A high-priority graphics queue specifically for the swap chain.
    OmegaGTE::GECommandQueueDesc presentDesc{};
    presentDesc.type = OmegaGTE::GECommandQueueDesc::Type::Graphics;
    presentDesc.priority = OmegaGTE::GECommandQueueDesc::Priority::High;
    presentDesc.maxBufferCount = 8;
    presentDesc.label = "Present";
    auto presentQueue = gte.graphicsEngine->makeCommandQueue(presentDesc);

The command buffer surface
==========================

A command buffer is a recording of GPU work. The same buffer can hold a
render pass, a compute pass, a blit pass, and an acceleration-structure
pass — open and close each in turn, in any order, then submit the whole
thing.

Most of this surface is documented next to the operation it performs:

* Render passes — :doc:`RenderPipeline`.
* Compute passes — :doc:`ComputePipeline`.
* Blit operations — :doc:`Blitting`.
* Raytracing passes — :doc:`Raytracing`.

The lifecycle methods below are shared by every pass.

.. cpp:namespace-push:: OmegaGTE::GECommandBuffer

.. cpp:function:: void reset()

    Clear all recorded commands and pass state. Must be called after
    the GPU has finished consuming the buffer, before re-recording
    into it. (Resetting a buffer the GPU is still using is undefined.)

.. cpp:function:: void setName(OmegaCommon::StrRef name)

    Debug label shown in GPU profilers. Set once after acquiring the
    buffer.

.. cpp:function:: void setCompletionHandler(const GECommandBufferCompletionHandler & handler)

    Register a callback invoked when the GPU finishes this buffer.
    See *Completion callbacks* below.

.. cpp:namespace-pop::

The command buffer also exposes a handful of *cross-pass copy* helpers
that do not need a render or compute pipeline — buffer-to-buffer,
buffer-to-texture, texture-to-buffer, and ``fillBuffer`` (a fast 32-bit
fill). See :doc:`Blitting` for the full signatures and discussion;
they sit on :cpp:class:`GECommandBuffer` so any pass-less command buffer
can use them.

Fences and cross-queue synchronisation
======================================

.. cpp:class:: OmegaGTE::GEFence

    A 64-bit GPU timeline counter. Created with
    :cpp:func:`OmegaGraphicsEngine::makeFence`. One queue signals it as it
    completes a buffer; any queue (or the CPU) can wait on the
    signal.

    .. cpp:function:: std::uint64_t getLastSignaledValue() const

        The most recently signalled value, as the GPU has advanced it.
        Use this as the target for :cpp:func:`waitForFence` to wait for
        "whatever has happened so far".

The canonical "queue A produces, queue B consumes" pattern:

.. code-block:: cpp

    auto fence = gte.graphicsEngine->makeFence();

    // Queue A: write into a texture, signal the fence on completion.
    auto producerCmd = queueA->getAvailableBuffer();
    // …encode a pass that writes targetTexture…
    queueA->submitCommandBuffer(producerCmd, fence);
    queueA->commitToGPU();

    // Queue B: wait for the fence at the start of its buffer, then sample.
    auto consumerCmd = queueB->getAvailableBuffer();
    // …encode a pass that samples targetTexture…
    queueB->notifyCommandBuffer(consumerCmd, fence);
    queueB->submitCommandBuffer(consumerCmd);
    queueB->commitToGPU();

    // CPU-side: block until queue A's last submission has actually finished.
    queueA->waitForFence(fence, fence->getLastSignaledValue());

The "I just blocked the CPU to wait, now wake up downstream queues" idiom:

.. code-block:: cpp

    // Some serial CPU work that needs the GPU to be at a known point.
    queueA->commitToGPUAndWait();
    queueA->signalFence(handoffFence);
    // queueB can now safely consume resources queueA produced.

Completion callbacks
====================

Attach a callback to a command buffer to learn when the GPU has finished
executing it. The callback runs on a backend-internal thread shortly after
completion — keep the work inside it cheap or punt to your own thread
pool.

.. cpp:struct:: OmegaGTE::GECommandBufferCompletionInfo

    Passed to the completion handler.

    .. cpp:type:: CompletionStatus

        +-----------------+--------------------------------------+
        | ``Completed``   | The buffer finished cleanly.         |
        +-----------------+--------------------------------------+
        | ``Error``       | The backend reported a GPU error.    |
        +-----------------+--------------------------------------+

    .. cpp:member:: CompletionStatus status

    .. cpp:member:: double gpuStartTimeSec
    .. cpp:member:: double gpuEndTimeSec

        GPU timeline timestamps in seconds. Reported only when the device
        advertises ``GTEDEVICE_FEATURE_TIMESTAMP_QUERIES``; otherwise both
        are ``0.0``.

.. cpp:type:: OmegaGTE::GECommandBufferCompletionHandler

    Alias for ``std::function<void(const GECommandBufferCompletionInfo &)>``.

.. code-block:: cpp

    cmd->setCompletionHandler([](const OmegaGTE::GECommandBufferCompletionInfo & info) {
        if (info.status == OmegaGTE::GECommandBufferCompletionInfo::CompletionStatus::Error) {
            std::cerr << "GPU error on command buffer.\n";
            return;
        }
        double ms = (info.gpuEndTimeSec - info.gpuStartTimeSec) * 1000.0;
        std::cout << "Frame GPU time: " << ms << " ms\n";
    });

Common pitfalls
===============

* **Calling** ``reset()`` **while the GPU is still using the buffer.** Wait
  on a fence or call ``commitToGPUAndWait`` first. The cheapest way to
  recycle one buffer per frame is to keep a fence per slot.
* **Cross-queue resource access without a fence.** Reading a texture on
  queue B that queue A wrote earlier the same frame, without a
  ``notifyCommandBuffer`` between them, races. The first frame may even
  look right; the symptoms appear under load.
* **Forgetting to** ``commitToGPU`` **(or ``commitToGPUAndWait``).** Submitted
  buffers sit in the queue until you commit. Nothing reaches the GPU.
* **Running expensive work inside a completion handler.** It runs on a
  backend thread. Hop to your own thread for anything non-trivial.
* **Treating** ``timestampPeriod`` **as device-independent.** GPU
  timestamps need ``GTEDEVICE_FEATURE_TIMESTAMP_QUERIES`` and use the
  device's tick rate — convert against
  :cpp:member:`GTEDeviceFeatures::timestampPeriod` if you read raw
  ticks.
