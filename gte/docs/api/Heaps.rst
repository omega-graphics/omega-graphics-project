=====
Heaps
=====

A heap is a pre-allocated pool of GPU memory that buffers and textures can
be sub-allocated from. Each call to ``OmegaGraphicsEngine::makeBuffer`` or
``makeTexture`` normally goes to the OS / driver allocator, which can be a
five-to-ten-microsecond round-trip per resource. When a subsystem needs to
spin up many small short-lived resources — a particle system rebuilding
its state buffer every frame, a UI batcher allocating a vertex buffer per
draw call — that overhead is the bottleneck. A heap lets the subsystem
take one allocator round-trip up front and then sub-allocate from a flat
pool that lives in one contiguous block of GPU memory.

There are two reasons to reach for a heap:

* **Allocator overhead.** Many small resources amortise the OS allocation
  cost across all of them.
* **Memory locality.** Resources allocated from the same heap tend to land
  near each other in device memory, which can help cache behaviour for
  workloads that touch them together.

If you only allocate a handful of large, long-lived resources, you do not
need a heap — call ``graphicsEngine->makeBuffer`` / ``makeTexture``
directly.

.. contents:: On this page
   :local:
   :depth: 2

GEHeap
======

.. cpp:class:: OmegaGTE::GEHeap

    A pre-allocated pool. The pool's total size is fixed at creation;
    individual sub-allocations come out of it via ``makeBuffer`` /
    ``makeTexture``. Sub-allocated resources hold strong references to the
    heap, so the heap stays alive as long as anything is allocated from it.

    .. cpp:function:: size_t currentSize()

        The number of bytes currently allocated from the heap. Use this to
        notice when you are about to run out — sub-allocation fails when
        the requested resource would push ``currentSize() > len``.

    .. cpp:function:: SharedHandle<GEBuffer> makeBuffer(const BufferDescriptor & desc)

        Allocates a buffer from this heap rather than from the device's
        default heap. See :doc:`Buffers` for the descriptor fields.

    .. cpp:function:: SharedHandle<GETexture> makeTexture(const TextureDescriptor & desc)

        Allocates a texture from this heap. See :doc:`Textures` for the
        descriptor fields.

.. cpp:struct:: OmegaGTE::HeapDescriptor

    .. cpp:type:: HeapType

        How the heap manages residency between CPU- and GPU-visible memory.

        +-----------------+-------------------------------------------------------+
        | ``Shared``      | The heap is visible to both CPU and GPU. Use for      |
        |                 | resources you update from the CPU each frame          |
        |                 | (per-frame uniform buffers, dynamic vertex buffers).  |
        +-----------------+-------------------------------------------------------+
        | ``Automatic``   | The heap manages residency itself, moving pages       |
        |                 | between CPU- and GPU-visible memory as access         |
        |                 | patterns dictate. Use when the resources have a       |
        |                 | mixed lifetime or you do not want to think about it.  |
        +-----------------+-------------------------------------------------------+

    .. cpp:member:: size_t len

        Total heap size in bytes. Sub-allocations come out of this budget.
        Pick a number that comfortably exceeds the peak working set; you
        cannot grow a heap after creation.

Allocating from a heap
======================

.. code-block:: cpp

    // 64 MB pool for a particle system's transient buffers.
    OmegaGTE::HeapDescriptor heapDesc;
    heapDesc.len = 64 * 1024 * 1024;
    auto particleHeap = gte.graphicsEngine->makeHeap(heapDesc);

    // 100 000 particle state slots (vec4)
    OmegaGTE::BufferDescriptor stateDesc;
    stateDesc.usage        = OmegaGTE::BufferDescriptor::GPUOnly;
    stateDesc.len          = 100000 * sizeof(float) * 4;
    stateDesc.objectStride = sizeof(float) * 4;
    auto particleState = particleHeap->makeBuffer(stateDesc);

    // A small GPU-only texture used as a particle gradient lookup,
    // also out of the same pool.
    OmegaGTE::TextureDescriptor lutDesc;
    lutDesc.kind        = OmegaGTE::TextureKind::Tex1D;
    lutDesc.usage       = OmegaGTE::GETexture::GPUAccessOnly;
    lutDesc.pixelFormat = OmegaGTE::PixelFormat::RGBA8Unorm;
    lutDesc.width       = 256;
    auto gradientLUT = particleHeap->makeTexture(lutDesc);

    std::cout << "Heap used: " << particleHeap->currentSize() << " / "
              << heapDesc.len << " bytes\n";

Common pitfalls
===============

* **Sizing the heap too small.** Sub-allocation fails (returns ``nullptr``)
  the moment ``currentSize() + requested > len``. There is no resize; the
  fix is to recreate the heap larger and re-allocate.
* **Holding sub-allocations past the heap's lifetime.** Resources keep the
  heap alive, but if you destroy the heap explicitly while sub-allocations
  still exist, the resource handles dangle.
* **Reaching for a heap when you only allocate a few resources.** The
  overhead saving is in batching many small allocations. One vertex buffer
  and one texture for a passport-photo demo are not worth a heap.
* **Mixing very-long-lived and per-frame resources in the same heap.** The
  long-lived ones fragment the pool. Use separate heaps for "scene"-scoped
  vs. "frame"-scoped resources.
