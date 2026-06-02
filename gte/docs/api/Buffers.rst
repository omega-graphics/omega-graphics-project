=======
Buffers
=======

A buffer is the universal container for typed, linear data on the GPU. Vertex
positions, indices, per-frame constants, particle states, compute inputs, and
compute outputs are all buffers: a contiguous block of GPU memory whose
elements share a fixed byte stride. Most of what makes a buffer subtle is not
the allocation itself — it is choosing the right *usage* (who reads and writes
it), the right *role* (how a shader sees it), and getting the per-platform
struct layout right when you copy structured data into it. This page walks
through those decisions in the order you make them.

.. contents:: On this page
   :local:
   :depth: 2

The buffer lifecycle
====================

Every buffer goes through the same five-step lifecycle, and each step maps to
one of the types documented below. Read this list once before diving into the
reference — it is the spine the rest of this page hangs on.

1. **Describe.** Fill in a :cpp:struct:`OmegaGTE::BufferDescriptor` — how big
   the buffer is, how big each element is, and how it will be used.
2. **Allocate.** Pass the descriptor to ``gte.graphicsEngine->makeBuffer(desc)``
   (or to a :doc:`heap <Heaps>`'s ``makeBuffer``). You get back a
   ``SharedHandle<GEBuffer>`` that owns the GPU allocation for the lifetime of
   the handle.
3. **Fill.** For an ``Upload`` buffer, copy structured data in with
   :cpp:struct:`OmegaGTE::GEBufferWriter`. The writer respects the platform's
   shader memory-layout rules so the bytes land where the shader expects to
   read them. For a ``GPUOnly`` or ``Readback`` buffer this step is done by a
   compute pass instead.
4. **Bind and consume.** Bind the buffer to a render or compute pass at an
   OmegaSL register (see :doc:`RenderPipeline` and :doc:`ComputePipeline`).
   The shader reads it as a ``buffer<T>`` or ``uniform<T>`` depending on the
   *role* you gave the descriptor.
5. **Read back (optional).** If the buffer was created with ``Usage::Readback``,
   use :cpp:struct:`OmegaGTE::GEBufferReader` after the GPU work has completed
   to pull the bytes back into CPU memory with the correct layout.

The two decisions you make at step 1 — usage and role — drive everything else,
so it is worth taking them apart.

Picking a usage
===============

``BufferDescriptor::Usage`` declares which side of the CPU/GPU boundary writes
the buffer and which side reads it. The backend uses this to pick the right
memory heap (shared CPU+GPU memory vs. GPU-private), to decide whether to
allocate a staging buffer, and to set the resource transitions required for
each access. Picking the wrong usage either silently degrades performance or
trips a validation error at copy time, so it pays to be deliberate here.

+-------------------+-----------------------------------------------------------+
| ``Upload``        | The CPU writes once (or per frame), the GPU reads.        |
|                   | Use for vertex buffers, index buffers, per-frame          |
|                   | constants, anything you stream in from C++.               |
+-------------------+-----------------------------------------------------------+
| ``Readback``      | The GPU writes, the CPU reads. Use for compute outputs    |
|                   | you want to inspect from C++, screenshots, GPU-side       |
|                   | profiling data.                                           |
+-------------------+-----------------------------------------------------------+
| ``GPUOnly``       | Both the producer and the consumer are on the GPU; the    |
|                   | CPU never touches the contents after creation. Use for    |
|                   | intermediate compute buffers, particle state advanced     |
|                   | by a compute pass, BVH inputs. Fastest at sample time     |
|                   | because the allocation lives in device-local memory.      |
+-------------------+-----------------------------------------------------------+

If you are unsure, ``Upload`` is the safe default — it is the only one where
the writer/reader on the CPU side is meaningful for filling the buffer.

Picking a role
==============

``BufferDescriptor::Role`` is independent of usage. It declares *how the
shader sees the buffer*, which maps to two different OmegaSL declarations and
two different memory-layout standards:

+---------------+----------------------------------------------------------------+
| ``Storage``   | The shader declares ``buffer<T>``. Lowers to                   |
| (default)     | ``StructuredBuffer<T>`` on D3D12, an SSBO on Vulkan, and       |
|               | ``device T*`` on Metal. Memory layout is **std430** —          |
|               | natural element alignment with the usual vec3 quirk            |
|               | (a ``vec3`` is treated as if it were a ``vec4`` for            |
|               | alignment). Use this for everything that is read or            |
|               | written as a typed array of structs: vertex buffers,           |
|               | particle pools, mesh data, indirect command buffers.           |
+---------------+----------------------------------------------------------------+
| ``Uniform``   | The shader declares ``uniform<T>``. Lowers to ``cbuffer``      |
|               | on D3D12, a UBO on Vulkan, and ``constant T &`` on             |
|               | Metal. Memory layout is **std140** on D3D12/Vulkan —           |
|               | matrix columns round up to 16 bytes and the whole              |
|               | struct rounds to a 16-byte multiple. Metal uses the            |
|               | natural layout (so on Metal, Uniform and Storage have          |
|               | the same byte size). Use this for small per-frame              |
|               | constants the shader reads many times: view/projection         |
|               | matrices, material parameters, lighting state.                 |
+---------------+----------------------------------------------------------------+

The role is load-bearing on D3D12 and Vulkan. A Uniform-shaped buffer on
D3D12 is padded to 256-byte alignment at allocation time, and on Vulkan it
gets ``VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT`` set on the underlying
``VkBuffer``. If you bind a ``Storage`` buffer to a slot the shader declared
as ``uniform<T>`` (or vice versa), bind validation rejects it — the role you
gave the descriptor is checked against the binding slot's kind at submit time.

The role also affects how strides are computed; see *Computing struct strides*
below.

Allocating a buffer
===================

.. cpp:struct:: OmegaGTE::BufferDescriptor

    Describes a buffer to allocate.

    .. cpp:member:: Usage usage

        Which side writes and which side reads. See *Picking a usage* above.
        Default ``Upload``.

    .. cpp:member:: size_t len

        Total size of the buffer in bytes. Typically ``elementCount *
        objectStride``.

    .. cpp:member:: size_t objectStride

        The size in bytes of one element. For a buffer of typed structs this
        is the per-element stride the shader will index by; use
        :cpp:func:`OmegaGTE::omegaSLStructStride` to compute it correctly for
        the role you chose. For a buffer of plain scalars (e.g. a ``float``
        readback buffer) it can be ``sizeof(float)``.

    .. cpp:member:: StorageOpts opts

        ``Shared`` (visible from both CPU and GPU) or ``GPUOnly`` (device
        memory). Most usages default to the correct option; you only need to
        override this for unusual cases.

    .. cpp:member:: Role role

        ``Storage`` (default) or ``Uniform``. See *Picking a role* above. This
        field was added after the original positional aggregate initialiser
        was in use, so the older ``BufferDescriptor{usage, len, stride}`` form
        still compiles and defaults to ``Storage``.

Once you have a descriptor, allocate the buffer through the engine:

.. code-block:: cpp

    // A vertex buffer for 1000 of our MyVertex structs (storage-role).
    OmegaGTE::BufferDescriptor vertDesc;
    vertDesc.usage        = OmegaGTE::BufferDescriptor::Upload;
    vertDesc.role         = OmegaGTE::BufferDescriptor::Storage;
    vertDesc.objectStride = OmegaGTE::omegaSLStructStride(
        { OMEGASL_DATA_FLOAT4, OMEGASL_DATA_FLOAT2 });   // pos + uv
    vertDesc.len          = 1000 * vertDesc.objectStride;
    auto vertexBuffer = gte.graphicsEngine->makeBuffer(vertDesc);

    // A small uniform buffer for per-frame view/projection matrices.
    OmegaGTE::BufferDescriptor uniDesc;
    uniDesc.usage        = OmegaGTE::BufferDescriptor::Upload;
    uniDesc.role         = OmegaGTE::BufferDescriptor::Uniform;   // <-- uniform<T>
    uniDesc.objectStride = OmegaGTE::omegaSLStructStride(
        { OMEGASL_DATA_FLOAT4X4, OMEGASL_DATA_FLOAT4X4 },
        OmegaGTE::BufferDescriptor::Uniform);                     // role-matched stride
    uniDesc.len          = uniDesc.objectStride;
    auto frameUniforms = gte.graphicsEngine->makeBuffer(uniDesc);

    // A GPU-only intermediate for a compute pass to fill.
    OmegaGTE::BufferDescriptor mid;
    mid.usage        = OmegaGTE::BufferDescriptor::GPUOnly;
    mid.len          = 1024 * sizeof(float);
    mid.objectStride = sizeof(float);
    auto intermediate = gte.graphicsEngine->makeBuffer(mid);

    // A readback buffer for a compute pass to write into; the CPU will then read it.
    OmegaGTE::BufferDescriptor rb;
    rb.usage        = OmegaGTE::BufferDescriptor::Readback;
    rb.len          = 1024 * sizeof(float);
    rb.objectStride = sizeof(float);
    auto readbackBuffer = gte.graphicsEngine->makeBuffer(rb);

The :cpp:class:`OmegaGTE::GEBuffer` itself is opaque — it owns the GPU
allocation and exposes only the size:

.. cpp:class:: OmegaGTE::GEBuffer

    A GPU buffer resource. Corresponds to ``buffer<T>`` (Storage role) or
    ``uniform<T>`` (Uniform role) in OmegaSL.

    .. cpp:function:: size_t size()

        Returns the total allocated size in bytes. Equals ``len`` from the
        descriptor, possibly rounded up for backend alignment.

    .. cpp:member:: BufferDescriptor::Role role

        The role the buffer was created with. Stored on the buffer so the
        bind path can verify it matches the binding slot's kind.

Filling a buffer with `GEBufferWriter`
======================================

The trap most newcomers hit is filling a buffer with ``memcpy``. It is fast,
familiar, and almost always wrong. The byte layout an OmegaSL shader expects
for a struct is **not** the byte layout your C++ compiler picks — it depends
on the binding role (std430 vs std140), it depends on the backend (Metal's
constant buffers diverge from D3D12/Vulkan), and it depends on the specific
field types (``vec3`` is padded to 16 bytes for alignment under std140 / std430,
matrix columns round up to 16 bytes under std140). Writing the bytes yourself
will silently misalign the data, and you will see the wrong vertex in the
wrong place rather than a clean error.

:cpp:struct:`OmegaGTE::GEBufferWriter` exists so you never have to think about
that. You declare each field of each struct, in order, and the writer emits
the bytes the shader expects.

The pattern is always the same:

.. code-block:: cpp

    struct MyVertex {
        OmegaGTE::FVec<4> pos;   // declared as float4 in the shader
        OmegaGTE::FVec<2> uv;    // declared as float2 in the shader
    };

    std::vector<MyVertex> vertices = { /* … */ };

    auto writer = OmegaGTE::GEBufferWriter::Create();
    writer->setOutputBuffer(vertexBuffer);

    for (auto & v : vertices) {
        writer->structBegin();
        writer->writeFloat4(v.pos);
        writer->writeFloat2(v.uv);
        writer->structEnd();
        writer->sendToBuffer();
    }
    writer->flush();

Three things to notice:

* ``structBegin()`` / ``structEnd()`` bracket exactly one struct. The field
  ``write*`` calls between them must appear in **declaration order** and the
  set of types must match the shader-side struct.
* ``sendToBuffer()`` advances the write cursor by one ``objectStride``. If
  you forget it, every struct overwrites the previous one and only the last
  one survives.
* ``flush()`` is what makes the bytes visible to the GPU. The writer batches
  internally and a missing ``flush()`` is the most common reason a buffer
  "looks right at write time but is garbage at draw time".

The full method surface:

.. cpp:struct:: OmegaGTE::GEBufferWriter

    Serialises C++ structs into a :cpp:class:`GEBuffer` with the correct
    OmegaSL memory layout. The writer is buffer-role aware: the bytes it
    emits already account for std430 (Storage) or std140 (Uniform) padding.
    Returned by :cpp:func:`Create`.

    .. cpp:function:: void setOutputBuffer(SharedHandle<GEBuffer> & buffer)

        Binds the destination buffer. Sticky — you can write many structs to
        the same buffer without rebinding.

    .. cpp:function:: void structBegin()
    .. cpp:function:: void structEnd()

        Bracket the per-field ``write*`` calls that describe one struct.

    .. cpp:function:: void sendToBuffer()

        Appends the current struct to the buffer and advances the write
        cursor by ``objectStride``.

    .. cpp:function:: void flush()

        Commits pending writes to GPU-visible memory. Call before submitting
        the command buffer that consumes the data.

    .. cpp:function:: static SharedHandle<GEBufferWriter> Create()

        Factory.

    **Scalar / vector writes** — match these to the OmegaSL scalar / vector
    types the shader declares for each field:

    .. cpp:function:: void writeFloat(float & v)
    .. cpp:function:: void writeFloat2(FVec<2> & v)
    .. cpp:function:: void writeFloat3(FVec<3> & v)
    .. cpp:function:: void writeFloat4(FVec<4> & v)
    .. cpp:function:: void writeInt(int & v)
    .. cpp:function:: void writeInt2(IVec<2> & v)
    .. cpp:function:: void writeInt3(IVec<3> & v)
    .. cpp:function:: void writeInt4(IVec<4> & v)
    .. cpp:function:: void writeUint(unsigned & v)
    .. cpp:function:: void writeUint2(UVec<2> & v)
    .. cpp:function:: void writeUint3(UVec<3> & v)
    .. cpp:function:: void writeUint4(UVec<4> & v)

    **Matrix writes** — both square and rectangular shapes are supported.
    The writer pads columns to match std430 / std140 column-alignment rules
    so the shader reads each column from the address it expects:

    .. cpp:function:: void writeFloat2x2(FMatrix<2,2> & m)
    .. cpp:function:: void writeFloat3x3(FMatrix<3,3> & m)
    .. cpp:function:: void writeFloat4x4(FMatrix<4,4> & m)
    .. cpp:function:: void writeFloat2x3(FMatrix<2,3> & m)
    .. cpp:function:: void writeFloat2x4(FMatrix<2,4> & m)
    .. cpp:function:: void writeFloat3x2(FMatrix<3,2> & m)
    .. cpp:function:: void writeFloat3x4(FMatrix<3,4> & m)
    .. cpp:function:: void writeFloat4x2(FMatrix<4,2> & m)
    .. cpp:function:: void writeFloat4x3(FMatrix<4,3> & m)

    The integer and unsigned-integer matrix variants
    (``writeInt2x2``…``writeInt4x3``, ``writeUint2x2``…``writeUint4x3``)
    share the same byte layout as the float versions — ``int``, ``unsigned``,
    and ``float`` are all 4-byte scalars under std430 / std140. In shaders,
    the integer variants bind to ``intCxR`` / ``uintCxR``, which the
    backends lower to an array of column vectors (``int4 m[C]``).

Computing struct strides
========================

The stride of one struct in a buffer depends on the role you chose, because
std430 and std140 have different padding rules. Call
:cpp:func:`OmegaGTE::omegaSLStructStride` with the field types and the role
to get the right number for ``BufferDescriptor::objectStride``:

.. cpp:function:: size_t OmegaGTE::omegaSLStructStride(\
    OmegaCommon::Vector<omegasl_data_type> fields,\
    BufferDescriptor::Role role = BufferDescriptor::Storage) noexcept

    Returns the byte size of one OmegaSL struct with the given field types,
    accounting for the layout rules of the supplied role. Pass the same role
    you give the buffer's descriptor so the allocation size matches what the
    writer packs.

.. code-block:: cpp

    // Storage role — std430 column-major. A vec3 still occupies 16 bytes
    // because of the std430 vec3-alignment quirk.
    size_t storageStride = OmegaGTE::omegaSLStructStride(
        { OMEGASL_DATA_FLOAT4, OMEGASL_DATA_FLOAT2 });

    // Uniform role — std140. A float4x4 occupies 64 bytes (no extra column
    // padding) but smaller shapes round up: a float3x3 is padded to 48 bytes.
    size_t uniformStride = OmegaGTE::omegaSLStructStride(
        { OMEGASL_DATA_FLOAT4X4, OMEGASL_DATA_FLOAT3X3 },
        OmegaGTE::BufferDescriptor::Uniform);

If the stride you pass to the descriptor disagrees with what the writer
packs, only the first element will be at the right address — every
subsequent element will be misaligned by the difference.

Reading data back with `GEBufferReader`
=======================================

A ``Readback`` buffer is filled by the GPU (via a compute pass that binds it
as an output) and consumed by the CPU after the work completes. The reader
is the mirror of the writer: declare the shader-side struct layout once, then
walk through each element pulling the fields out in declaration order.

The wait-for-completion step is not done by the reader. Either submit with
``commitToGPUAndWait`` or attach a fence to the producer command buffer and
block on it (see :doc:`GPUSubmission`); the reader assumes the bytes are
already there.

.. code-block:: cpp

    // After a compute pass has written results into readbackBuffer …
    auto reader = OmegaGTE::GEBufferReader::Create();
    reader->setInputBuffer(readbackBuffer);
    reader->setStructLayout({ OMEGASL_DATA_FLOAT4 });

    std::vector<OmegaGTE::FVec<4>> results;
    for (int i = 0; i < resultCount; ++i) {
        OmegaGTE::FVec<4> value;
        reader->structBegin();
        reader->getFloat4(value);
        reader->structEnd();
        results.push_back(value);
    }

.. cpp:struct:: OmegaGTE::GEBufferReader

    Reads OmegaSL-layout struct data back from a ``Readback`` buffer into CPU
    memory, stripping any std140 / std430 padding the writer added.

    .. cpp:function:: void setInputBuffer(SharedHandle<GEBuffer> & buffer)

        Binds the source buffer.

    .. cpp:function:: void setStructLayout(OmegaCommon::Vector<omegasl_data_type> fields)

        Declares the field types of one element so the reader can apply
        correct strides between elements.

    .. cpp:function:: void structBegin()
    .. cpp:function:: void structEnd()

        Bracket the per-field ``get*`` calls for one struct.

    .. cpp:function:: void reset()

        Rewinds the read cursor to the start of the buffer.

    .. cpp:function:: static SharedHandle<GEBufferReader> Create()

        Factory.

    **Scalar / vector reads** — mirror the writer's ``write*`` set:

    .. cpp:function:: void getFloat(float & v)
    .. cpp:function:: void getFloat2(FVec<2> & v)
    .. cpp:function:: void getFloat3(FVec<3> & v)
    .. cpp:function:: void getFloat4(FVec<4> & v)
    .. cpp:function:: void getInt(int & v)
    .. cpp:function:: void getInt2(IVec<2> & v)
    .. cpp:function:: void getInt3(IVec<3> & v)
    .. cpp:function:: void getInt4(IVec<4> & v)
    .. cpp:function:: void getUint(unsigned & v)
    .. cpp:function:: void getUint2(UVec<2> & v)
    .. cpp:function:: void getUint3(UVec<3> & v)
    .. cpp:function:: void getUint4(UVec<4> & v)

    **Matrix reads** — symmetric with the writer; the reader strips the
    column padding when copying into the host's tightly-packed ``FMatrix``,
    ``IMatrix``, or ``UMatrix``:

    .. cpp:function:: void getFloat2x2(FMatrix<2,2> & m)
    .. cpp:function:: void getFloat3x3(FMatrix<3,3> & m)
    .. cpp:function:: void getFloat4x4(FMatrix<4,4> & m)
    .. cpp:function:: void getFloat2x3(FMatrix<2,3> & m)
    .. cpp:function:: void getFloat2x4(FMatrix<2,4> & m)
    .. cpp:function:: void getFloat3x2(FMatrix<3,2> & m)
    .. cpp:function:: void getFloat3x4(FMatrix<3,4> & m)
    .. cpp:function:: void getFloat4x2(FMatrix<4,2> & m)
    .. cpp:function:: void getFloat4x3(FMatrix<4,3> & m)

    Integer and unsigned-integer matrix reads (``getInt2x2``…``getInt4x3``,
    ``getUint2x2``…``getUint4x3``) follow the same shape and layout.

Common pitfalls
===============

* **Forgetting** ``flush()``. The buffer looks correct in C++ but the shader
  sees uninitialised or stale memory. Always flush before submitting the
  command buffer that consumes the data.
* **Stride / role mismatch.** Compute ``objectStride`` with the same role
  you set on the descriptor; otherwise the second and later elements land at
  the wrong offsets.
* **Binding role disagreement.** A ``Storage`` buffer bound to a ``uniform<T>``
  slot (or vice versa) is rejected at submit time. Match the shader
  declaration.
* **Reading before the GPU finishes.** ``GEBufferReader`` does not wait. Use
  ``commitToGPUAndWait`` or a fence to synchronise before reading. See
  :doc:`GPUSubmission`.
* **memcpy into the GEBuffer.** There is no direct ``data()`` pointer on
  ``GEBuffer`` for a reason — the writer is the path that gets the layout
  right. Do not look for a back door.
