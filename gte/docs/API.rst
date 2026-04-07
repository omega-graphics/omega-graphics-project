===
API
===

This page documents every public class, struct, and free function in OmegaGTE.
All symbols live in the ``OmegaGTE`` namespace. Include the umbrella header to
pull in everything::

    #include <OmegaGTE.h>

.. contents:: On this page
   :local:
   :depth: 2

----

Initialization
--------------

The ``GTE`` struct
~~~~~~~~~~~~~~~~~~

.. cpp:struct:: OmegaGTE::GTE

    The top-level handle returned by ``Init()`` or ``InitWithDefaultDevice()``.
    It owns the graphics and triangulation engine instances for the lifetime of
    the application.

    .. cpp:member:: SharedHandle<OmegaGraphicsEngine> graphicsEngine

        The graphics engine. Use this to create buffers, textures, pipelines,
        render targets, and command queues.

    .. cpp:member:: SharedHandle<OmegaTriangulationEngine> triangulationEngine

        The triangulation engine. Use this to convert geometric shapes into
        triangle meshes that can be drawn with the graphics engine.

    .. cpp:member:: SharedHandle<OmegaSLCompiler> omegaSlCompiler

        Available when ``RUNTIME_SHADER_COMP_SUPPORT`` is defined. Provides
        in-process OmegaSL compilation. See :ref:`runtime-compilation`.

Device Enumeration
~~~~~~~~~~~~~~~~~~

.. cpp:struct:: OmegaGTE::GTEDeviceFeatures

    Reports the optional capabilities of a ``GTEDevice``.

    .. cpp:member:: bool raytracing

        ``true`` if the device supports hardware ray tracing.

    .. cpp:member:: bool msaa4x

        ``true`` if 4× MSAA is supported.

    .. cpp:member:: bool msaa8x

        ``true`` if 8× MSAA is supported.

.. cpp:struct:: OmegaGTE::GTEDevice

    Represents a single GPU (or software rasteriser). Do not construct this
    directly — obtain instances from ``enumerateDevices()``.

    .. cpp:member:: const Type type

        Either ``GTEDevice::Integrated`` or ``GTEDevice::Discrete``.

    .. cpp:member:: const OmegaCommon::String name

        Human-readable device name reported by the OS / driver.

    .. cpp:member:: const GTEDeviceFeatures features

        The feature set of this device.

    .. cpp:function:: const void *native()

        Returns the underlying platform handle: ``ID3D12Device *`` on Windows,
        ``id<MTLDevice>`` on Apple platforms, ``VkDevice`` on Vulkan.

.. cpp:function:: OmegaCommon::Vector<SharedHandle<GTEDevice>> OmegaGTE::enumerateDevices()

    Returns every GPU visible to the OS. Iterate over the list to pick a
    device by ``type`` or ``features`` before calling ``Init()``.

.. code-block:: cpp

    auto devices = OmegaGTE::enumerateDevices();

    // Prefer a discrete GPU if available
    SharedHandle<OmegaGTE::GTEDevice> chosen = devices.front();
    for (auto & dev : devices) {
        if (dev->type == OmegaGTE::GTEDevice::Discrete) {
            chosen = dev;
            break;
        }
    }

    // Print name and capabilities
    std::cout << chosen->name << "\n";
    if (chosen->features.raytracing)
        std::cout << "  ray tracing: yes\n";
    if (chosen->features.msaa8x)
        std::cout << "  MSAA 8x:     yes\n";

Starting and stopping OmegaGTE
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. cpp:function:: GTE OmegaGTE::Init(SharedHandle<GTEDevice> & device)

    Initialises OmegaGTE for a specific device and returns the ``GTE`` handle.

.. cpp:function:: GTE OmegaGTE::InitWithDefaultDevice()

    Initialises OmegaGTE using the system's default (best available) device.

.. cpp:function:: void OmegaGTE::Close(GTE & gte)

    Shuts down both engines and releases all associated GPU resources. Call
    this before the process exits.

.. code-block:: cpp

    // Option A — choose a device yourself
    auto devices = OmegaGTE::enumerateDevices();
    auto device  = devices[0];
    OmegaGTE::GTE gte = OmegaGTE::Init(device);

    // Option B — let OmegaGTE choose
    OmegaGTE::GTE gte = OmegaGTE::InitWithDefaultDevice();

    // … use gte.graphicsEngine / gte.triangulationEngine …

    OmegaGTE::Close(gte);

----

Shader Libraries
----------------

.. _runtime-compilation:

Shaders are compiled from ``.omegasl`` source files by the ``omegaslc`` compiler
(see OmegaSL docs) and packaged into ``.omegasllib`` archives. OmegaGTE loads
these archives and returns individual ``GTEShader`` objects that are referenced
when building pipeline states.

.. cpp:struct:: OmegaGTE::GTEShaderLibrary

    A loaded shader archive. Contains a map of shader name → ``GTEShader``.

    .. cpp:member:: std::map<std::string, SharedHandle<GTEShader>> shaders

        Keyed by the function name declared in OmegaSL source.

.. cpp:struct:: OmegaGTE::GTEShader

    An opaque compiled shader entry point. Pass to ``RenderPipelineDescriptor``
    or ``ComputePipelineDescriptor`` to wire it into a pipeline.

Loading a library
~~~~~~~~~~~~~~~~~

.. code-block:: cpp

    // Load from disk
    auto shaderLib = gte.graphicsEngine->loadShaderLibrary("assets/myShaders.omegasllib");

    // Retrieve named entry points
    auto vertShader = shaderLib->shaders["myVertex"];
    auto fragShader = shaderLib->shaders["myFragment"];
    auto compShader = shaderLib->shaders["myCompute"];

Runtime compilation
~~~~~~~~~~~~~~~~~~~

When ``RUNTIME_SHADER_COMP_SUPPORT`` is defined the runtime compiler is
available as ``gte.omegaSlCompiler``. See the OmegaSL documentation for
the ``OmegaSLCompiler`` API.

.. code-block:: cpp

    #ifdef RUNTIME_SHADER_COMP_SUPPORT
    std::string src = R"(
        vertex VertexRaster myVert(uint vid : VertexID){ … }
        fragment float4 myFrag(VertexRaster r){ … }
    )";
    auto runtimeLib = gte.omegaSlCompiler->compile(src, "myShaders.omegasl");
    auto shaderLib  = gte.graphicsEngine->loadShaderLibraryRuntime(runtimeLib);
    #endif

----

GPU Resources
-------------

The graphics engine is the factory for all GPU resources. Provide a descriptor
struct, get back a ``SharedHandle<>`` that keeps the resource alive as long as
at least one handle exists.

Pixel Formats
~~~~~~~~~~~~~

The ``PixelFormat`` enum is used when creating textures, render targets, and
pipelines.

+-----------------------------+------------------------------------------------------+
| Value                       | Description                                          |
+=============================+======================================================+
| ``RGBA8Unorm``              | 8-bit RGBA, linear (default for most textures)       |
+-----------------------------+------------------------------------------------------+
| ``RGBA16Unorm``             | 16-bit RGBA, linear                                  |
+-----------------------------+------------------------------------------------------+
| ``RGBA8Unorm_SRGB``         | 8-bit RGBA, gamma-encoded sRGB                       |
+-----------------------------+------------------------------------------------------+
| ``BGRA8Unorm``              | 8-bit BGRA, linear (typical swap-chain format)       |
+-----------------------------+------------------------------------------------------+
| ``BGRA8Unorm_SRGB``         | 8-bit BGRA, gamma-encoded sRGB                       |
+-----------------------------+------------------------------------------------------+

GEBuffer
~~~~~~~~

.. cpp:class:: OmegaGTE::GEBuffer

    A block of GPU memory whose elements all share the same stride.
    Corresponds to ``buffer<T>`` in OmegaSL.

    .. cpp:function:: size_t size()

        Returns the total allocated size in bytes.

.. cpp:struct:: OmegaGTE::BufferDescriptor

    Describes the buffer to allocate.

    **Usage**

    +-------------------+---------------------------------------------------------------+
    | ``Upload``        | CPU writes → GPU reads. Use for vertex, index, constant data. |
    +-------------------+---------------------------------------------------------------+
    | ``Readback``      | GPU writes → CPU reads. Use for compute output or screenshots.|
    +-------------------+---------------------------------------------------------------+
    | ``GPUOnly``       | Resident only on the GPU. Fastest for resources the CPU never |
    |                   | touches after initial upload.                                 |
    +-------------------+---------------------------------------------------------------+

    .. cpp:member:: Usage usage

        Default ``Upload``.

    .. cpp:member:: size_t len

        Total buffer size in bytes.

    .. cpp:member:: size_t objectStride

        The size in bytes of each element. Used for indexed access in OmegaSL.

    .. cpp:member:: StorageOpts opts

        ``Shared`` (CPU + GPU accessible) or ``GPUOnly``.

.. code-block:: cpp

    // Vertex buffer — 1000 vertices, each 32 bytes
    OmegaGTE::BufferDescriptor vertDesc;
    vertDesc.usage        = OmegaGTE::BufferDescriptor::Upload;
    vertDesc.len          = 1000 * 32;
    vertDesc.objectStride = 32;
    auto vertexBuffer = gte.graphicsEngine->makeBuffer(vertDesc);

    // GPU-only output buffer for a compute shader
    OmegaGTE::BufferDescriptor outDesc;
    outDesc.usage        = OmegaGTE::BufferDescriptor::GPUOnly;
    outDesc.len          = 1024 * sizeof(float);
    outDesc.objectStride = sizeof(float);
    auto outputBuffer = gte.graphicsEngine->makeBuffer(outDesc);

    // Readback buffer — read results back to CPU after a compute pass
    OmegaGTE::BufferDescriptor rbDesc;
    rbDesc.usage        = OmegaGTE::BufferDescriptor::Readback;
    rbDesc.len          = 1024 * sizeof(float);
    rbDesc.objectStride = sizeof(float);
    auto readbackBuffer = gte.graphicsEngine->makeBuffer(rbDesc);

Uploading data with GEBufferWriter
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. cpp:struct:: OmegaGTE::GEBufferWriter

    Serialises C++ structs into a ``GEBuffer`` with correct OmegaSL memory
    layout. Use this instead of raw ``memcpy`` to respect platform alignment
    and padding rules.

    .. cpp:function:: void setOutputBuffer(SharedHandle<GEBuffer> & buffer)

        Binds the destination buffer.

    .. cpp:function:: void structBegin()

        Begins writing one struct element.

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

        Write one field of the struct. Call these in declaration order between
        ``structBegin()`` and ``structEnd()``.

    .. cpp:function:: void structEnd()

        Finalises one struct element.

    .. cpp:function:: void sendToBuffer()

        Appends the current struct to the buffer and advances the write cursor.

    .. cpp:function:: void flush()

        Commits all pending writes to GPU memory.

    .. cpp:function:: static SharedHandle<GEBufferWriter> Create()

        Factory function. Returns a new writer instance.

.. code-block:: cpp

    struct MyVertex {
        OmegaGTE::FVec<4> pos;   // float4
        OmegaGTE::FVec<2> uv;    // float2
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

Reading data with GEBufferReader
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. cpp:struct:: OmegaGTE::GEBufferReader

    Reads OmegaSL-layout struct data back from a ``Readback`` buffer to CPU
    memory.

    .. cpp:function:: void setInputBuffer(SharedHandle<GEBuffer> & buffer)

        Binds the source buffer.

    .. cpp:function:: void setStructLayout(OmegaCommon::Vector<omegasl_data_type> fields)

        Declares the OmegaSL data types of each field so the reader can apply
        correct strides.

    .. cpp:function:: void structBegin()

        Begins reading one struct element.

    .. cpp:function:: void getFloat(float & v)
    .. cpp:function:: void getFloat2(FVec<2> & v)
    .. cpp:function:: void getFloat3(FVec<3> & v)
    .. cpp:function:: void getFloat4(FVec<4> & v)

        Read one field into the referenced variable.

    .. cpp:function:: void structEnd()

        Finalises reading the current struct.

    .. cpp:function:: void reset()

        Rewinds the read cursor to the start of the buffer.

    .. cpp:function:: static SharedHandle<GEBufferReader> Create()

        Factory function.

.. code-block:: cpp

    // After a compute pass has written results to readbackBuffer …
    auto reader = OmegaGTE::GEBufferReader::Create();
    reader->setInputBuffer(readbackBuffer);
    reader->setStructLayout({ OMEGASL_DATA_FLOAT4 });  // one float4 per struct

    float4Results.clear();
    for (int i = 0; i < resultCount; i++) {
        OmegaGTE::FVec<4> value;
        reader->structBegin();
        reader->getFloat4(value);
        reader->structEnd();
        float4Results.push_back(value);
    }

Struct size helper
~~~~~~~~~~~~~~~~~~

.. cpp:function:: size_t OmegaGTE::omegaSLStructSize(OmegaCommon::Vector<omegasl_data_type> fields)

    Returns the byte size of an OmegaSL struct with the given field types,
    accounting for platform alignment. Use this to compute ``objectStride``
    when creating a ``BufferDescriptor``.

.. code-block:: cpp

    // Struct in OmegaSL: { float4 pos; float2 uv; }
    size_t stride = OmegaGTE::omegaSLStructSize(
        { OMEGASL_DATA_FLOAT4, OMEGASL_DATA_FLOAT2 });

    OmegaGTE::BufferDescriptor desc;
    desc.usage        = OmegaGTE::BufferDescriptor::Upload;
    desc.len          = vertexCount * stride;
    desc.objectStride = stride;

GETexture
~~~~~~~~~

.. cpp:class:: OmegaGTE::GETexture

    A GPU texture resource. Corresponds to ``texture1d``, ``texture2d``, or
    ``texture3d`` in OmegaSL.

    **Texture types** — ``GETexture::GETextureType``

    +-------------------+-------------------------+
    | ``Texture1D``     | One-dimensional texture |
    +-------------------+-------------------------+
    | ``Texture2D``     | Two-dimensional texture |
    +-------------------+-------------------------+
    | ``Texture3D``     | Three-dimensional/volume texture |
    +-------------------+---------------------------+

    **Texture usages** — ``GETexture::GETextureUsage``

    +-------------------------------+------------------------------------------------------+
    | ``ToGPU``                     | CPU → GPU upload. Use for static texture assets.     |
    +-------------------------------+------------------------------------------------------+
    | ``FromGPU``                   | GPU → CPU readback. Use to retrieve computed pixels. |
    +-------------------------------+------------------------------------------------------+
    | ``GPUAccessOnly``             | GPU-private; never accessed by the CPU.              |
    +-------------------------------+------------------------------------------------------+
    | ``RenderTarget``              | Can be used as a render target output.               |
    +-------------------------------+------------------------------------------------------+
    | ``MSResolveSrc``              | Source texture for MSAA resolve operations.          |
    +-------------------------------+------------------------------------------------------+
    | ``RenderTargetAndDepthStencil`` | Combined colour + depth/stencil render target.     |
    +-------------------------------+------------------------------------------------------+

    .. cpp:function:: void copyBytes(void *bytes, size_t bytesPerRow)

        Uploads raw pixel data from CPU memory. Only valid for ``ToGPU`` textures.
        ``bytesPerRow`` is ``width * bytesPerPixel``.

    .. cpp:function:: size_t getBytes(void *bytes, size_t bytesPerRow)

        Downloads pixel data to ``bytes``. Only valid for ``FromGPU`` textures.
        Pass ``nullptr`` to query the required buffer size without reading.

.. cpp:struct:: OmegaGTE::TextureDescriptor

    Describes a texture to allocate.

    .. cpp:member:: GETexture::GETextureType type

    .. cpp:member:: GETexture::GETextureUsage usage

        Default ``ToGPU``.

    .. cpp:member:: TexturePixelFormat pixelFormat

        Default ``RGBA8Unorm``.

    .. cpp:member:: unsigned width
    .. cpp:member:: unsigned height
    .. cpp:member:: unsigned depth

        Default ``1``. Set to > 1 for 3D textures.

    .. cpp:member:: unsigned mipLevels

        Default ``1``. Increase to enable mipmapping.

    .. cpp:member:: unsigned sampleCount

        Default ``1``. Set to ``4`` or ``8`` for MSAA (requires device support).

.. cpp:struct:: OmegaGTE::TextureRegion

    A sub-region of a texture used in blit copy operations.

    .. cpp:member:: unsigned x, y, z

        Origin of the region.

    .. cpp:member:: unsigned w, h, d

        Dimensions of the region.

.. code-block:: cpp

    // 2D diffuse texture — upload from CPU, read-only on GPU
    OmegaGTE::TextureDescriptor texDesc;
    texDesc.type        = OmegaGTE::GETexture::Texture2D;
    texDesc.usage       = OmegaGTE::GETexture::ToGPU;
    texDesc.pixelFormat = OmegaGTE::PixelFormat::RGBA8Unorm;
    texDesc.width       = 512;
    texDesc.height      = 512;
    auto diffuseTex = gte.graphicsEngine->makeTexture(texDesc);

    // Upload RGBA pixel data
    diffuseTex->copyBytes(pixelData, 512 * 4);  // 4 bytes per pixel

    // Off-screen render target texture (GPU-only)
    OmegaGTE::TextureDescriptor rtDesc;
    rtDesc.type        = OmegaGTE::GETexture::Texture2D;
    rtDesc.usage       = OmegaGTE::GETexture::RenderTarget;
    rtDesc.pixelFormat = OmegaGTE::PixelFormat::RGBA8Unorm;
    rtDesc.width       = 1920;
    rtDesc.height      = 1080;
    auto renderTex = gte.graphicsEngine->makeTexture(rtDesc);

    // Read computed pixels back to CPU
    OmegaGTE::TextureDescriptor readDesc;
    readDesc.type        = OmegaGTE::GETexture::Texture2D;
    readDesc.usage       = OmegaGTE::GETexture::FromGPU;
    readDesc.pixelFormat = OmegaGTE::PixelFormat::RGBA8Unorm;
    readDesc.width  = 256;
    readDesc.height = 256;
    auto readbackTex = gte.graphicsEngine->makeTexture(readDesc);

    size_t needed = readbackTex->getBytes(nullptr, 256 * 4);
    std::vector<uint8_t> pixels(needed);
    readbackTex->getBytes(pixels.data(), 256 * 4);

GESamplerState
~~~~~~~~~~~~~~

.. cpp:class:: OmegaGTE::GESamplerState

    An opaque sampler object that controls how a texture is filtered and
    addressed during sampling in a shader. Bind to a shader using
    ``bindResourceAtFragmentShader`` or ``bindResourceAtVertexShader``.

.. cpp:struct:: OmegaGTE::SamplerDescriptor

    Describes a sampler to create.

    .. cpp:member:: OmegaCommon::StrRef name

        Optional debug name.

    .. cpp:member:: AddressMode uAddressMode
    .. cpp:member:: AddressMode vAddressMode
    .. cpp:member:: AddressMode wAddressMode

        Addressing behaviour for each texture axis. Default ``Wrap``.

        +------------------------------+--------------------------------------------+
        | ``Wrap``                     | Tile the texture                           |
        +------------------------------+--------------------------------------------+
        | ``ClampToEdge``              | Stretch the edge pixel                     |
        +------------------------------+--------------------------------------------+
        | ``MirrorClampToEdge``        | Mirror once, then clamp                    |
        +------------------------------+--------------------------------------------+
        | ``MirrorWrap``               | Alternate flipped / normal tiles           |
        +------------------------------+--------------------------------------------+

    .. cpp:member:: Filter filter

        +--------------------------------------+----------------------------------------------+
        | ``Linear``                           | Bilinear / trilinear filtering               |
        +--------------------------------------+----------------------------------------------+
        | ``Point``                            | Nearest-neighbour                            |
        +--------------------------------------+----------------------------------------------+
        | ``MaxAnisotropic``                   | Anisotropic (max quality)                    |
        +--------------------------------------+----------------------------------------------+
        | ``MinAnisotropic``                   | Anisotropic (min quality)                    |
        +--------------------------------------+----------------------------------------------+
        | ``MagLinearMinPointMipLinear``       | Mixed mag/min/mip filtering                  |
        +--------------------------------------+----------------------------------------------+

    .. cpp:member:: unsigned int maxAnisotropy

        Anisotropy ratio cap (default 16).

.. code-block:: cpp

    OmegaGTE::SamplerDescriptor sampDesc;
    sampDesc.name         = "linearWrap";
    sampDesc.filter       = OmegaGTE::SamplerDescriptor::Filter::Linear;
    sampDesc.uAddressMode = OmegaGTE::SamplerDescriptor::AddressMode::Wrap;
    sampDesc.vAddressMode = OmegaGTE::SamplerDescriptor::AddressMode::Wrap;
    auto linearSampler = gte.graphicsEngine->makeSamplerState(sampDesc);

    // Anisotropic sampler for high-quality terrain texturing
    OmegaGTE::SamplerDescriptor anisoDesc;
    anisoDesc.name           = "anisotropic16";
    anisoDesc.filter         = OmegaGTE::SamplerDescriptor::Filter::MaxAnisotropic;
    anisoDesc.maxAnisotropy  = 16;
    anisoDesc.uAddressMode   = OmegaGTE::SamplerDescriptor::AddressMode::Wrap;
    anisoDesc.vAddressMode   = OmegaGTE::SamplerDescriptor::AddressMode::Wrap;
    auto anisoSampler = gte.graphicsEngine->makeSamplerState(anisoDesc);

GEHeap
~~~~~~

.. cpp:class:: OmegaGTE::GEHeap

    A pre-allocated pool of GPU memory from which buffers and textures can be
    sub-allocated. Useful when creating many small resources to reduce
    allocation overhead and improve memory locality.

    .. cpp:function:: size_t currentSize()

        Returns the number of bytes currently allocated from the heap.

    .. cpp:function:: SharedHandle<GEBuffer> makeBuffer(const BufferDescriptor & desc)

        Allocates a buffer from this heap.

    .. cpp:function:: SharedHandle<GETexture> makeTexture(const TextureDescriptor & desc)

        Allocates a texture from this heap.

.. cpp:struct:: OmegaGTE::HeapDescriptor

    .. cpp:member:: size_t len

        Total heap size in bytes.

.. code-block:: cpp

    // Reserve 64 MB for a particle system's buffers and textures
    OmegaGTE::HeapDescriptor heapDesc;
    heapDesc.len = 64 * 1024 * 1024;
    auto particleHeap = gte.graphicsEngine->makeHeap(heapDesc);

    OmegaGTE::BufferDescriptor pBufDesc;
    pBufDesc.usage = OmegaGTE::BufferDescriptor::GPUOnly;
    pBufDesc.len   = 100000 * sizeof(float) * 4;
    auto particleBuffer = particleHeap->makeBuffer(pBufDesc);

GEFence
~~~~~~~

.. cpp:class:: OmegaGTE::GEFence

    A GPU timeline fence used to synchronise work between different command
    queues. One queue signals the fence; another waits on it.

    .. cpp:function:: std::uint64_t getLastSignaledValue() const

        Returns the most recently signalled value. Use this on the CPU to
        check whether the GPU has reached a particular point in a queue.

.. code-block:: cpp

    auto fence = gte.graphicsEngine->makeFence();

    // Queue A produces a texture then signals the fence
    queueA->submitCommandBuffer(producerCmd, fence);
    queueA->commitToGPU();

    // Queue B waits for the fence before consuming the texture
    queueB->notifyCommandBuffer(consumerCmd, fence);
    queueB->submitCommandBuffer(consumerCmd);
    queueB->commitToGPU();

    // CPU-side wait — block until the fence is signalled
    queueA->waitForFence(fence, fence->getLastSignaledValue());

----

Pipelines
---------

Render Pipeline
~~~~~~~~~~~~~~~

A render pipeline state bundles the vertex and fragment shaders with fixed-
function rasteriser settings. Create it once and reuse across frames.

.. cpp:struct:: OmegaGTE::RenderPipelineDescriptor

    .. cpp:member:: OmegaCommon::String name

        Debug label shown in GPU profilers.

    .. cpp:member:: SharedHandle<GTEShader> vertexFunc

        The vertex shader entry point.

    .. cpp:member:: SharedHandle<GTEShader> fragmentFunc

        The fragment shader entry point.

    .. cpp:member:: PixelFormat colorPixelFormat

        Must match the pixel format of the render target. Default ``RGBA8Unorm``.

    .. cpp:member:: unsigned rasterSampleCount

        Number of MSAA samples. ``0`` or ``1`` disables MSAA.

    .. cpp:member:: RasterCullMode cullMode

        +------------------+---------------------------------------------------+
        | ``None``         | Draw all triangles regardless of winding order   |
        +------------------+---------------------------------------------------+
        | ``Front``        | Discard front-facing triangles                   |
        +------------------+---------------------------------------------------+
        | ``Back``         | Discard back-facing triangles (default for 3D)   |
        +------------------+---------------------------------------------------+

    .. cpp:member:: TriangleFillMode triangleFillMode

        ``Solid`` (default) or ``Wireframe``.

    .. cpp:member:: GTEPolygonFrontFaceRotation polygonFrontFaceRotation

        ``Clockwise`` (default) or ``CounterClockwise`` — controls which
        winding order is considered "front-facing".

    **Depth & Stencil** (``depthAndStencilDesc``)

    .. cpp:member:: bool enableDepth

        Enable depth testing. Default ``false``.

    .. cpp:member:: bool enableStencil

        Enable stencil testing. Default ``false``.

    .. cpp:member:: DepthWriteAmount writeAmount

        ``All`` (default) writes depth; ``Zero`` prevents depth writes.

    .. cpp:member:: CompareFunc depthOperation

        Depth comparison function. ``Less`` passes fragments closer to the
        camera (default). Other options: ``LessEqual``, ``Greater``,
        ``GreaterEqual``.

    .. cpp:member:: float depthBias, slopeScale, depthClamp

        Depth bias parameters for shadow mapping and z-fighting mitigation.

    .. cpp:member:: unsigned stencilReadMask, stencilWriteMask

        Masks applied to the stencil buffer during read and write.

    **Stencil operations** (``frontFaceStencil``, ``backFaceStencil``)

    Each stencil desc contains ``stencilFail``, ``depthFail``, and ``pass``
    operations: ``Retain``, ``Zero``, ``Replace``, ``IncrementWrap``,
    ``DecrementWrap``. The comparison function is set via ``func``.

.. code-block:: cpp

    auto shaderLib = gte.graphicsEngine->loadShaderLibrary("shaders.omegasllib");

    OmegaGTE::RenderPipelineDescriptor pipeDesc;
    pipeDesc.name             = "MainPipeline";
    pipeDesc.vertexFunc       = shaderLib->shaders["myVertex"];
    pipeDesc.fragmentFunc     = shaderLib->shaders["myFragment"];
    pipeDesc.colorPixelFormat = OmegaGTE::PixelFormat::BGRA8Unorm;
    pipeDesc.cullMode         = OmegaGTE::RasterCullMode::Back;

    // Enable depth testing
    pipeDesc.depthAndStencilDesc.enableDepth    = true;
    pipeDesc.depthAndStencilDesc.depthOperation = OmegaGTE::CompareFunc::Less;
    pipeDesc.depthAndStencilDesc.writeAmount    = OmegaGTE::DepthWriteAmount::All;

    auto pipeline = gte.graphicsEngine->makeRenderPipelineState(pipeDesc);

    // Wireframe pipeline for debugging
    OmegaGTE::RenderPipelineDescriptor wirePipeDesc = pipeDesc;
    wirePipeDesc.name             = "WireframePipeline";
    wirePipeDesc.triangleFillMode = OmegaGTE::TriangleFillMode::Wireframe;
    auto wirePipeline = gte.graphicsEngine->makeRenderPipelineState(wirePipeDesc);

Compute Pipeline
~~~~~~~~~~~~~~~~

.. cpp:struct:: OmegaGTE::ComputePipelineDescriptor

    .. cpp:member:: OmegaCommon::StrRef name

        Debug label.

    .. cpp:member:: SharedHandle<GTEShader> computeFunc

        The compute shader entry point.

.. code-block:: cpp

    OmegaGTE::ComputePipelineDescriptor compDesc;
    compDesc.name        = "ParticleUpdate";
    compDesc.computeFunc = shaderLib->shaders["updateParticles"];
    auto computePipeline = gte.graphicsEngine->makeComputePipelineState(compDesc);

----

Render Targets
--------------

A render target is the destination surface for GPU drawing. The two variants —
native window targets and texture targets — share the same ``GERenderTarget``
command-buffer API.

GENativeRenderTarget
~~~~~~~~~~~~~~~~~~~~

.. cpp:class:: OmegaGTE::GENativeRenderTarget

    Renders directly into a platform window (swap chain). Supports
    ``commitAndPresent()`` to flip the back buffer to the screen.

    .. cpp:function:: PixelFormat pixelFormat()

        Returns ``BGRA8Unorm`` — the standard swap-chain format.

    .. cpp:function:: void commitAndPresent()

        Submits all recorded commands to the GPU and presents the result on
        screen. Call this at the end of every frame.

    *Windows-only additional methods:*

    .. cpp:function:: void *getSwapChain()

        Returns the ``IDXGISwapChain3 *`` for direct swap-chain manipulation.

    .. cpp:function:: void resizeSwapChain(unsigned int width, unsigned int height)

        Resizes the swap chain. Call this in response to WM_SIZE instead of
        calling ``IDXGISwapChain::ResizeBuffers`` directly.

    .. cpp:function:: void waitForGPU()

        Blocks the CPU until the GPU has finished all work on this target.
        Required before releasing window resources.

    .. cpp:function:: void waitForFence(SharedHandle<GEFence> & fence)

        CPU wait for a specific fence signal before using a texture produced
        by another queue.

.. cpp:struct:: OmegaGTE::NativeRenderTargetDescriptor

    Platform-specific fields. Fill only the fields for your target platform.

    .. cpp:member:: bool allowDepthStencilTesting

        Must be ``true`` to enable depth/stencil testing in the pipeline.
        Default ``false``.

    *DirectX (Windows):*

    .. cpp:member:: bool isHwnd
    .. cpp:member:: HWND hwnd
    .. cpp:member:: unsigned width
    .. cpp:member:: unsigned height

    *Metal (macOS/iOS — Objective-C only):*

    .. cpp:member:: CAMetalLayer *metalLayer

    *Vulkan (Linux/Android):*

    ``x_window`` / ``x_display`` for X11, ``wl_surface`` / ``wl_display`` +
    ``width`` / ``height`` for Wayland, ``window`` for Android.

.. code-block:: cpp

    // Windows example
    OmegaGTE::NativeRenderTargetDescriptor nativeDesc;
    nativeDesc.isHwnd                = true;
    nativeDesc.hwnd                  = hWnd;
    nativeDesc.width                 = clientWidth;
    nativeDesc.height                = clientHeight;
    nativeDesc.allowDepthStencilTesting = true;
    auto nativeRT = gte.graphicsEngine->makeNativeRenderTarget(nativeDesc);

    // Resize the swap chain when the window is resized (Windows)
    nativeRT->resizeSwapChain(newWidth, newHeight);

GETextureRenderTarget
~~~~~~~~~~~~~~~~~~~~~

.. cpp:class:: OmegaGTE::GETextureRenderTarget

    Renders to an off-screen ``GETexture``. Use for shadow maps, post-process
    effects, environment probes, and any other off-screen rendering pass.

    .. cpp:function:: void commit()

        Submits recorded commands to the GPU (no screen flip).

    .. cpp:function:: SharedHandle<GETexture> underlyingTexture()

        Returns the texture that received the rendered output.

    .. cpp:function:: void waitForGPU()

        CPU wait until all GPU work on this render target completes. Call
        before releasing a pooled texture.

    .. cpp:function:: void signalFence(SharedHandle<GEFence> & fence)

        Signal a fence after all texture work is done (e.g. after
        ``waitForGPU()``). Another queue waiting on this fence can then safely
        sample the texture.

.. cpp:struct:: OmegaGTE::TextureRenderTargetDescriptor

    .. cpp:member:: bool renderToExistingTexture

        When ``true``, ``texture`` is used as the target; OmegaGTE does not
        allocate a new texture.

    .. cpp:member:: SharedHandle<GETexture> texture

        The texture to render into (when ``renderToExistingTexture`` is
        ``true``).

    .. cpp:member:: TextureRegion region

        Sub-region of the texture to render into.

.. code-block:: cpp

    // Create a texture for the render target
    OmegaGTE::TextureDescriptor rtTexDesc;
    rtTexDesc.type        = OmegaGTE::GETexture::Texture2D;
    rtTexDesc.usage       = OmegaGTE::GETexture::RenderTarget;
    rtTexDesc.pixelFormat = OmegaGTE::PixelFormat::RGBA8Unorm;
    rtTexDesc.width       = 1024;
    rtTexDesc.height      = 1024;
    auto shadowMapTex = gte.graphicsEngine->makeTexture(rtTexDesc);

    // Create a render target that renders into that texture
    OmegaGTE::TextureRenderTargetDescriptor trtDesc;
    trtDesc.renderToExistingTexture = true;
    trtDesc.texture                 = shadowMapTex;
    trtDesc.region = { 0, 0, 0, 1024, 1024, 1 };
    auto shadowRT = gte.graphicsEngine->makeTextureRenderTarget(trtDesc);

    // After rendering, use the texture in the main pass
    // shadowMapTex can be bound as a shader resource

----

Command Queues
--------------

.. cpp:class:: OmegaGTE::GECommandQueue

    A fixed-size pool of command buffers submitted to the GPU as a group.
    All buffers in one queue execute sequentially in submission order.

    .. cpp:function:: SharedHandle<GECommandBuffer> getAvailableBuffer()

        Returns the next unused command buffer in the pool. Call ``reset()``
        on a buffer after it completes before reusing it.

    .. cpp:function:: unsigned getSize()

        Returns the total number of command buffers in the pool.

    .. cpp:function:: void notifyCommandBuffer(SharedHandle<GECommandBuffer> & commandBuffer, SharedHandle<GEFence> & waitFence)

        Encodes a fence wait at the beginning of ``commandBuffer`` so it
        stalls on the GPU until ``waitFence`` is signalled by another queue.

    .. cpp:function:: void submitCommandBuffer(SharedHandle<GECommandBuffer> & commandBuffer)

        Enqueues the command buffer for execution. No inter-queue sync.

    .. cpp:function:: void submitCommandBuffer(SharedHandle<GECommandBuffer> & commandBuffer, SharedHandle<GEFence> & signalFence)

        Enqueues and signals ``signalFence`` when the buffer completes.

    .. cpp:function:: void commitToGPU()

        Submits all enqueued buffers to the GPU without waiting.

    .. cpp:function:: void commitToGPUAndWait()

        Submits all enqueued buffers and blocks until the GPU finishes them.

    .. cpp:function:: void signalExternalFence(SharedHandle<GEFence> & fence)

        Signals a fence on this queue without a command buffer (useful after
        ``waitForGPU()`` to notify other queues).

    .. cpp:function:: void waitForFence(SharedHandle<GEFence> & fence, std::uint64_t value)

        CPU-side wait — blocks until the fence reaches ``value``.

.. code-block:: cpp

    // Create a command queue that can hold up to 3 command buffers
    auto queue = gte.graphicsEngine->makeCommandQueue(3);

    auto cmdBuf = queue->getAvailableBuffer();
    cmdBuf->setName("FrameCommands");

    // … encode commands …

    queue->submitCommandBuffer(cmdBuf);
    queue->commitToGPUAndWait();

    // Reset before reusing
    cmdBuf->reset();

----

Command Buffers and Encoding
-----------------------------

There are two command buffer surfaces:

* ``GERenderTarget::CommandBuffer`` — the recommended high-level surface
  obtained directly from a render target. Supports render passes, compute
  passes, and blit passes.
* ``GECommandBuffer`` — the lower-level surface obtained from a
  ``GECommandQueue``. Exposes the same compute and blit API but the render
  pass methods are routed through ``GERenderTarget::CommandBuffer``.

Both share the same pass structure: you open a pass, record commands, close
the pass, then submit.

Render Pass
~~~~~~~~~~~

.. cpp:class:: OmegaGTE::GERenderTarget::CommandBuffer

    Obtained from ``renderTarget->commandBuffer()``.

    **Render pass**

    .. cpp:function:: void startRenderPass(const RenderPassDesc & desc)

        Opens a render pass. Must be called before any draw commands.

    .. cpp:function:: void setRenderPipelineState(SharedHandle<GERenderPipelineState> & pipelineState)

        Sets the active render pipeline.

    .. cpp:function:: void bindResourceAtVertexShader(SharedHandle<GEBuffer> & buffer, unsigned id)
    .. cpp:function:: void bindResourceAtVertexShader(SharedHandle<GETexture> & texture, unsigned id)

        Binds a resource to the vertex shader at OmegaSL register ``id``.

    .. cpp:function:: void bindResourceAtFragmentShader(SharedHandle<GEBuffer> & buffer, unsigned id)
    .. cpp:function:: void bindResourceAtFragmentShader(SharedHandle<GETexture> & texture, unsigned id)

        Binds a resource to the fragment shader at OmegaSL register ``id``.

    .. cpp:function:: void setViewports(std::vector<GEViewport> viewports)

        Dynamically sets the viewport(s) for this render pass.

    .. cpp:function:: void setScissorRects(std::vector<GEScissorRect> scissorRects)

        Dynamically sets the scissor rectangle(s) for this render pass.

    .. cpp:function:: void drawPolygons(PolygonType polygonType, unsigned vertexCount, size_t start)

        Encodes a draw call.

        +---------------------+----------------------------------------------+
        | ``Triangle``        | Each group of 3 vertices forms a triangle    |
        +---------------------+----------------------------------------------+
        | ``TriangleStrip``   | Vertices form a connected triangle strip     |
        +---------------------+----------------------------------------------+

    .. cpp:function:: void endRenderPass()

        Closes the render pass.

    **Compute pass** (within a render-target command buffer)

    .. cpp:function:: void startComputePass(SharedHandle<GEComputePipelineState> & computePipelineState)
    .. cpp:function:: void bindResourceAtComputeShader(SharedHandle<GEBuffer> & buffer, unsigned id)
    .. cpp:function:: void bindResourceAtComputeShader(SharedHandle<GETexture> & texture, unsigned id)
    .. cpp:function:: void dispatchThreadgroups(unsigned x, unsigned y, unsigned z)
    .. cpp:function:: void dispatchThreads(unsigned x, unsigned y, unsigned z)
    .. cpp:function:: void endComputePass()

    **Other**

    .. cpp:function:: void reset()

        Clears all recorded commands. Must be called after submission before
        re-recording into the same buffer.

    .. cpp:function:: void setCompletionHandler(GECommandBufferCompletionHandler handler)

        Registers a callback invoked when the GPU finishes executing this
        buffer. The callback receives a ``GECommandBufferCompletionInfo``
        with timing data (where supported).

.. cpp:struct:: OmegaGTE::GERenderTarget::RenderPassDesc

    Describes a render pass — what to do with the colour and depth buffers at
    the start of the pass.

    .. cpp:member:: ColorAttachment *colorAttachment

        Pointer to a ``ColorAttachment`` describing the load action and clear
        colour.

    .. cpp:member:: DepthStencilAttachment depthStencilAttachment

        Controls how the depth and stencil buffers are initialised.

    .. cpp:member:: bool multisampleResolve

        Set to ``true`` to resolve a multisampled texture at the end of the pass.

**ColorAttachment**

+--------------------+-------------------------------------------------------+
| ``Load``           | Keep the existing contents                           |
+--------------------+-------------------------------------------------------+
| ``LoadPreserve``   | Load and preserve (backend hint)                     |
+--------------------+-------------------------------------------------------+
| ``Clear``          | Fill with ``clearColor`` before rendering            |
+--------------------+-------------------------------------------------------+
| ``Discard``        | Discard the previous contents (fastest)              |
+--------------------+-------------------------------------------------------+

**DepthStencilAttachment**

Set ``disabled = false`` to activate. Separate ``depthloadAction`` and
``stencilLoadAction`` control each buffer. ``clearDepth`` defaults to ``1.0``
(far plane); ``clearStencil`` defaults to ``0``.

GEViewport and GEScissorRect
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. cpp:struct:: OmegaGTE::GEViewport

    Defines the 3-D viewport space mapped to the render target.

    .. cpp:member:: float x, y

        Upper-left corner of the viewport in pixel coordinates.

    .. cpp:member:: float width, height

        Viewport dimensions.

    .. cpp:member:: float nearDepth, farDepth

        Depth range, typically ``0.0`` (near) and ``1.0`` (far).

.. cpp:struct:: OmegaGTE::GEScissorRect

    A 2-D rectangle that clips the viewport. Fragments outside the rectangle
    are discarded before the fragment shader runs.

    .. cpp:member:: float x, y
    .. cpp:member:: float width, height

**Complete render-loop example**

.. code-block:: cpp

    // Each frame:
    auto cmdBuf = nativeRT->commandBuffer();

    // Colour attachment: clear to dark grey
    OmegaGTE::GERenderTarget::RenderPassDesc::ColorAttachment colorAttach(
        { 0.1f, 0.1f, 0.1f, 1.0f },
        OmegaGTE::GERenderTarget::RenderPassDesc::ColorAttachment::Clear
    );

    // Depth attachment: clear to 1.0 (far)
    OmegaGTE::GERenderTarget::RenderPassDesc::DepthStencilAttachment depthAttach;
    depthAttach.disabled        = false;
    depthAttach.depthloadAction = OmegaGTE::GERenderTarget::RenderPassDesc::DepthStencilAttachment::Clear;
    depthAttach.clearDepth      = 1.0f;

    OmegaGTE::GERenderTarget::RenderPassDesc passDesc;
    passDesc.colorAttachment          = &colorAttach;
    passDesc.depthStencilAttachment   = depthAttach;

    cmdBuf->startRenderPass(passDesc);

    cmdBuf->setRenderPipelineState(pipeline);

    // Full-screen viewport
    cmdBuf->setViewports({{ 0.f, 0.f, (float)width, (float)height, 0.f, 1.f }});

    // Bind vertex buffer at register 0, texture at register 1
    cmdBuf->bindResourceAtVertexShader(vertexBuffer,  0);
    cmdBuf->bindResourceAtFragmentShader(diffuseTex,  1);
    cmdBuf->bindResourceAtFragmentShader(linearSampler, 2);

    cmdBuf->drawPolygons(
        OmegaGTE::GERenderTarget::CommandBuffer::Triangle,
        vertexCount,
        0   // start index
    );

    cmdBuf->endRenderPass();

    nativeRT->submitCommandBuffer(cmdBuf);
    nativeRT->commitAndPresent();

    cmdBuf->reset();   // ready for the next frame

Compute Dispatch
~~~~~~~~~~~~~~~~

Compute passes are encoded on the same ``GERenderTarget::CommandBuffer`` or
on a ``GECommandBuffer`` from a queue.

.. cpp:function:: void GECommandBuffer::startComputePass(const GEComputePassDescriptor & desc)

    Opens a compute pass. The descriptor is currently empty; pass a
    default-constructed ``GEComputePassDescriptor{}``.

.. cpp:function:: void GECommandBuffer::setComputePipelineState(SharedHandle<GEComputePipelineState> & pipelineState)

    Sets the active compute pipeline.

.. cpp:function:: void GECommandBuffer::bindResourceAtComputeShader(SharedHandle<GEBuffer> & buffer, unsigned id)
.. cpp:function:: void GECommandBuffer::bindResourceAtComputeShader(SharedHandle<GETexture> & texture, unsigned id)

    Binds a resource to the compute shader at OmegaSL register ``id``.

.. cpp:function:: void GECommandBuffer::dispatchThreadgroups(unsigned x, unsigned y, unsigned z)

    Dispatches ``x * y * z`` thread groups. Each group contains the number of
    threads declared in the OmegaSL ``compute(x=N, y=M, z=P)`` descriptor.

.. cpp:function:: void GECommandBuffer::dispatchThreads(unsigned x, unsigned y, unsigned z)

    Dispatches exactly ``x * y * z`` threads. The backend divides by the
    threadgroup size internally. Use this when you know the total thread count
    rather than the group count.

.. cpp:function:: void GECommandBuffer::finishComputePass()

    Closes the compute pass.

.. code-block:: cpp

    // 1-D dispatch: double 1024 float values
    auto cmdBuf = queue->getAvailableBuffer();

    OmegaGTE::GEComputePassDescriptor compPassDesc{};
    cmdBuf->startComputePass(compPassDesc);
    cmdBuf->setComputePipelineState(computePipeline);
    cmdBuf->bindResourceAtComputeShader(inputBuffer,  0);
    cmdBuf->bindResourceAtComputeShader(outputBuffer, 1);

    // OmegaSL shader declares compute(x=64, y=1, z=1)
    // 1024 / 64 = 16 thread groups
    cmdBuf->dispatchThreadgroups(16, 1, 1);

    cmdBuf->finishComputePass();

    queue->submitCommandBuffer(cmdBuf);
    queue->commitToGPUAndWait();
    cmdBuf->reset();

    // Alternatively, dispatch by total thread count
    cmdBuf->startComputePass(compPassDesc);
    cmdBuf->setComputePipelineState(computePipeline);
    cmdBuf->bindResourceAtComputeShader(inputBuffer,  0);
    cmdBuf->bindResourceAtComputeShader(outputBuffer, 1);
    cmdBuf->dispatchThreads(1024, 1, 1);   // backend computes group count
    cmdBuf->finishComputePass();

    // 2-D image processing: 512 × 512 image, 8 × 8 thread groups
    cmdBuf->startComputePass(compPassDesc);
    cmdBuf->setComputePipelineState(blurPipeline);
    cmdBuf->bindResourceAtComputeShader(inputImage,  0);
    cmdBuf->bindResourceAtComputeShader(outputImage, 1);
    cmdBuf->dispatchThreadgroups(512 / 8, 512 / 8, 1);
    cmdBuf->finishComputePass();

Blit Pass
~~~~~~~~~

A blit pass copies data between textures on the GPU without involving shaders.

.. cpp:function:: void GECommandBuffer::startBlitPass()

    Opens a blit pass.

.. cpp:function:: void GECommandBuffer::copyTextureToTexture(SharedHandle<GETexture> & src, SharedHandle<GETexture> & dest)

    Copies the entire ``src`` texture into ``dest``.

.. cpp:function:: void GECommandBuffer::copyTextureToTexture(SharedHandle<GETexture> & src, SharedHandle<GETexture> & dest, const TextureRegion & region, const GPoint3D & destCoord)

    Copies a sub-``region`` of ``src`` into ``dest`` at origin ``destCoord``.

.. cpp:function:: void GECommandBuffer::finishBlitPass()

    Closes the blit pass.

.. code-block:: cpp

    // Copy a shadow map into a shadow atlas
    OmegaGTE::TextureRegion srcRegion{ 0, 0, 0, 1024, 1024, 1 };
    OmegaGTE::GPoint3D      destOrigin{ 1024.f, 0.f, 0.f };  // atlas column 1

    auto cmdBuf = queue->getAvailableBuffer();
    cmdBuf->startBlitPass();
    cmdBuf->copyTextureToTexture(shadowMapTex, atlasTexture, srcRegion, destOrigin);
    cmdBuf->finishBlitPass();

    queue->submitCommandBuffer(cmdBuf);
    queue->commitToGPU();

Completion Callbacks
~~~~~~~~~~~~~~~~~~~~

.. cpp:struct:: OmegaGTE::GECommandBufferCompletionInfo

    Passed to the completion handler when a command buffer finishes on the GPU.

    .. cpp:member:: CompletionStatus status

        ``Completed`` or ``Error``.

    .. cpp:member:: double gpuStartTimeSec
    .. cpp:member:: double gpuEndTimeSec

        GPU timeline timestamps in seconds (where the backend supports them).

.. code-block:: cpp

    cmdBuf->setCompletionHandler([](const OmegaGTE::GECommandBufferCompletionInfo & info) {
        if (info.status == OmegaGTE::GECommandBufferCompletionInfo::CompletionStatus::Error) {
            // handle GPU error
            return;
        }
        double gpuMs = (info.gpuEndTimeSec - info.gpuStartTimeSec) * 1000.0;
        printf("GPU time: %.2f ms\n", gpuMs);
    });

----

Raytracing
----------

.. note::

   Raytracing support is conditional. On Windows it requires Windows 10 1809
   or later. On Metal it requires macOS 11 / iOS 14. On Vulkan it is always
   available. The ``OMEGAGTE_RAYTRACING_SUPPORTED`` macro is defined when
   the feature is available.

Acceleration Structures
~~~~~~~~~~~~~~~~~~~~~~~

Ray tracing uses *acceleration structures* (BVH trees built by the GPU) to
efficiently find ray–geometry intersections.

.. cpp:struct:: OmegaGTE::GEAccelerationStructDescriptor

    Describes the geometry to accelerate.

    .. cpp:function:: void addTriangleBuffer(SharedHandle<GEBuffer> & buffer)

        Adds a triangle list (vertex buffer) as a geometry source.

    .. cpp:function:: void addBoundingBoxBuffer(SharedHandle<GEBuffer> & buffer)

        Adds an AABB (axis-aligned bounding box) buffer for procedural
        geometry.

.. cpp:struct:: OmegaGTE::GEAccelerationStruct

    An opaque GPU acceleration structure. Bind to a compute shader at an
    OmegaSL register with ``bindResourceAtComputeShader``.

.. cpp:struct:: OmegaGTE::GERaytracingBoundingBox

    A bounding box used for procedural (AABB-based) ray tracing geometry.

    .. cpp:member:: float minX, minY, minZ, maxX, maxY, maxZ

.. code-block:: cpp

    #ifdef OMEGAGTE_RAYTRACING_SUPPORTED

    // Build a bottom-level acceleration structure from a triangle mesh
    OmegaGTE::GEAccelerationStructDescriptor asDesc;
    asDesc.addTriangleBuffer(vertexBuffer);

    auto accelStruct = gte.graphicsEngine->allocateAccelerationStructure(asDesc);

    // Build the structure in a command buffer
    auto cmdBuf = queue->getAvailableBuffer();
    cmdBuf->beginAccelStructPass();
    cmdBuf->buildAccelerationStructure(accelStruct, asDesc);
    cmdBuf->finishAccelStructPass();

    queue->submitCommandBuffer(cmdBuf);
    queue->commitToGPUAndWait();

    // Bind the acceleration structure to a ray tracing compute shader
    auto rtBuf = queue->getAvailableBuffer();
    OmegaGTE::GEComputePassDescriptor rtDesc{};
    rtBuf->startComputePass(rtDesc);
    rtBuf->setComputePipelineState(raytracingPipeline);
    rtBuf->bindResourceAtComputeShader(accelStruct, 0);
    rtBuf->bindResourceAtComputeShader(outputImage,  1);
    rtBuf->dispatchRays(width, height, 1);
    rtBuf->finishComputePass();

    #endif

----

Triangulation Engine
--------------------

The Triangulation Engine converts high-level geometric descriptions (rectangles,
rounded rectangles, 3D primitives, vector paths) into indexed triangle meshes
ready to draw with the graphics engine.

OmegaTriangulationEngine
~~~~~~~~~~~~~~~~~~~~~~~~

.. cpp:class:: OmegaGTE::OmegaTriangulationEngine

    Top-level factory for triangulation contexts. Accessed via
    ``gte.triangulationEngine``.

    .. cpp:function:: SharedHandle<OmegaTriangulationEngineContext> createTEContextFromNativeRenderTarget(SharedHandle<GENativeRenderTarget> & renderTarget)

        Creates a context whose viewport is derived from the given window
        render target.

    .. cpp:function:: SharedHandle<OmegaTriangulationEngineContext> createTEContextFromTextureRenderTarget(SharedHandle<GETextureRenderTarget> & renderTarget)

        Creates a context whose viewport is derived from the given texture
        render target.

OmegaTriangulationEngineContext
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. cpp:class:: OmegaGTE::OmegaTriangulationEngineContext

    Performs triangulation. Create one per render target.

    .. cpp:function:: void setArcStep(float newArcStep)

        Controls the angular resolution used for curves and circles. Smaller
        values produce smoother arcs at the cost of more triangles. Default
        is ``0.01`` radians.

    .. cpp:function:: TETriangulationResult triangulateSync(const TETriangulationParams & params, GTEPolygonFrontFaceRotation frontFaceRotation, GEViewport * viewport)

        Triangulates on the calling thread and returns when complete.
        ``viewport`` may be ``nullptr`` to use the context's default viewport.

    .. cpp:function:: std::future<TETriangulationResult> triangulateAsync(const TETriangulationParams & params, GTEPolygonFrontFaceRotation frontFaceRotation, GEViewport * viewport)

        Triangulates on a background thread. Returns a ``std::future<>`` that
        resolves to the result.

    .. cpp:function:: std::future<TETriangulationResult> triangulateOnGPU(const TETriangulationParams & params, GTEPolygonFrontFaceRotation frontFaceRotation, GEViewport * viewport)

        Triangulates using a GPU compute pipeline. Returns a ``std::future<>``.
        Fastest for large meshes.

TETriangulationParams — Geometry Types
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

All factory functions return a ``TETriangulationParams`` that can be passed to
any of the three triangulation methods.

.. cpp:function:: static TETriangulationParams TETriangulationParams::Rect(GRect & rect)

    Triangulates a 2-D axis-aligned rectangle.

.. cpp:function:: static TETriangulationParams TETriangulationParams::RoundedRect(GRoundedRect & roundedRect)

    Triangulates a 2-D rectangle with rounded corners.

.. cpp:function:: static TETriangulationParams TETriangulationParams::RectangularPrism(GRectangularPrism & rectPrism)

    Triangulates a 3-D box.

.. cpp:function:: static TETriangulationParams TETriangulationParams::Pyramid(GPyramid & pyramid)

    Triangulates a 3-D pyramid.

.. cpp:function:: static TETriangulationParams TETriangulationParams::Ellipsoid(GEllipsoid & ellipsoid)

    Triangulates an ellipsoid (sphere when all radii are equal).

.. cpp:function:: static TETriangulationParams TETriangulationParams::Cylinder(GCylinder & cylinder)

    Triangulates a cylinder.

.. cpp:function:: static TETriangulationParams TETriangulationParams::Cone(GCone & cone)

    Triangulates a cone.

.. cpp:function:: static TETriangulationParams TETriangulationParams::GraphicsPath2D(GVectorPath2D & path, float strokeWidth, bool contour, bool fill)

    Triangulates a 2-D vector path.

    * ``strokeWidth`` — width of the stroke in world units.
    * ``contour`` — when ``true``, triangulate the outline only.
    * ``fill`` — when ``true``, triangulate the filled interior.

.. cpp:function:: static TETriangulationParams TETriangulationParams::GraphicsPath3D(unsigned vectorPathCount, GVectorPath3D * const vectorPaths)

    Triangulates one or more 3-D vector paths swept into a surface.

Attachments
~~~~~~~~~~~

Attachments associate colour or texture coordinate data with a triangulated
mesh so the resulting vertex buffer carries that data directly.

.. cpp:function:: void TETriangulationParams::addAttachment(const Attachment & attachment)

    Appends an attachment to the params.

.. cpp:struct:: OmegaGTE::TETriangulationParams::Attachment

    .. cpp:function:: static Attachment makeColor(const FVec<4> & color)

        Assigns a flat colour to all vertices.

    .. cpp:function:: static Attachment makeTexture2D(unsigned width, unsigned height)

        Assigns 2-D UV coordinates to each vertex.

    .. cpp:function:: static Attachment makeTexture3D(unsigned width, unsigned height, unsigned depth)

        Assigns 3-D UVW coordinates to each vertex.

TETriangulationResult
~~~~~~~~~~~~~~~~~~~~~~

.. cpp:struct:: OmegaGTE::TETriangulationResult

    Contains the triangulated output, ready to upload and render.

    .. cpp:member:: std::vector<TEMesh> meshes

        CPU-side mesh data, one ``TEMesh`` per geometry sub-section.

    .. cpp:function:: unsigned totalVertexCount()

        Returns the sum of all vertex counts across all meshes.

    .. cpp:function:: void translate(float x, float y, float z, const GEViewport & viewport)

        Translates all meshes and re-uploads the GPU vertex buffer.

    .. cpp:function:: void rotate(float pitch, float yaw, float roll)

        Rotates all meshes (in-place on the CPU; call after ``triangulateSync``
        to re-position before drawing).

    .. cpp:function:: void scale(float w, float h, float l)

        Scales all meshes.

**TEMesh**

Each ``TEMesh`` has a ``topology`` (``TopologyTriangle`` or
``TopologyTriangleStrip``), a vector of ``Polygon`` objects (each holding
three vertices ``a``, ``b``, ``c`` with optional ``AttachmentData``), and
per-mesh ``translate``, ``rotate``, and ``scale`` helpers.

Complete triangulation example
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: cpp

    auto teCtx = gte.triangulationEngine
                    ->createTEContextFromNativeRenderTarget(nativeRT);

    // Smooth curves at 0.005 rad steps
    teCtx->setArcStep(0.005f);

    // --- 2D rectangle with a colour attachment ---
    OmegaGTE::GRect rect{{ 0.1f, 0.1f }, 0.8f, 0.6f };
    auto rectParams = OmegaGTE::TETriangulationParams::Rect(rect);
    rectParams.addAttachment(
        OmegaGTE::TETriangulationParams::Attachment::makeColor(
            OmegaGTE::makeColor(0.2f, 0.6f, 1.0f, 1.0f)));

    auto rectResult = teCtx->triangulateSync(rectParams);

    // --- 3D sphere (unit ellipsoid) ---
    OmegaGTE::GEllipsoid sphere{ 0.f, 0.f, 0.f, 1.f, 1.f, 1.f };
    auto sphereParams  = OmegaGTE::TETriangulationParams::Ellipsoid(sphere);
    auto sphereResult  = teCtx->triangulateSync(sphereParams);
    sphereResult.translate(0.f, 0.f, -5.f, viewport);
    sphereResult.scale(2.f, 2.f, 2.f);

    // --- 2D vector path (a triangle outline) ---
    OmegaGTE::GVectorPath2D path({ 0.f, 0.5f });
    path.append({ -0.5f, -0.5f });
    path.append({  0.5f, -0.5f });
    auto pathParams = OmegaGTE::TETriangulationParams::GraphicsPath2D(
        path, 2.f, /*contour=*/true, /*fill=*/false);
    auto pathResult = teCtx->triangulateSync(pathParams);

    // --- GPU-accelerated triangulation (async) ---
    auto futureResult = teCtx->triangulateOnGPU(sphereParams);
    // … do other work …
    auto gpuSphereResult = futureResult.get();

    // --- Draw: bind the GPU vertex buffer in a render pass ---
    cmdBuf->startRenderPass(passDesc);
    cmdBuf->setRenderPipelineState(pipeline);
    cmdBuf->endRenderPass();

----

CPU Math Types
--------------

These types live on the CPU and are used to build the data passed to
``GEBufferWriter`` and to the Triangulation Engine.

Geometric Primitives
~~~~~~~~~~~~~~~~~~~~

.. cpp:struct:: OmegaGTE::GPoint2D

    A 2-D point.

    .. cpp:member:: float x, y

.. cpp:struct:: OmegaGTE::GPoint3D

    A 3-D point.

    .. cpp:member:: float x, y, z

.. cpp:struct:: OmegaGTE::GRect

    A 2-D axis-aligned rectangle.

    .. cpp:member:: GPoint2D pos

        Upper-left corner.

    .. cpp:member:: float w, h

        Width and height.

.. cpp:struct:: OmegaGTE::GRoundedRect

    A rectangle with uniform corner rounding.

    .. cpp:member:: GPoint2D pos
    .. cpp:member:: float w, h
    .. cpp:member:: float rad_x, rad_y

        Corner radii on the x and y axes.

.. cpp:struct:: OmegaGTE::GRectangularPrism

    A 3-D box.

    .. cpp:member:: GPoint3D pos
    .. cpp:member:: float w, h, d

.. cpp:struct:: OmegaGTE::GCylinder

    A cylinder aligned to the y-axis.

    .. cpp:member:: GPoint3D pos
    .. cpp:member:: float r

        Radius.

    .. cpp:member:: float h

        Height.

.. cpp:struct:: OmegaGTE::GPyramid

    A pyramid.

    .. cpp:member:: float x, y, z

        Base centre.

    .. cpp:member:: float w, d

        Base width and depth.

    .. cpp:member:: float h

        Height.

.. cpp:struct:: OmegaGTE::GCone

    A cone aligned to the y-axis.

    .. cpp:member:: float x, y, z
    .. cpp:member:: float r

        Base radius.

    .. cpp:member:: float h

        Height.

.. cpp:struct:: OmegaGTE::GEllipsoid

    An ellipsoid (use equal radii for a sphere).

    .. cpp:member:: float x, y, z

        Centre.

    .. cpp:member:: float rad_x, rad_y, rad_z

        Semi-axis radii.

.. cpp:struct:: OmegaGTE::GArc

    A 2-D arc.

    .. cpp:member:: GPoint2D center
    .. cpp:member:: float radians

        Sweep angle.

    .. cpp:member:: unsigned radius_x, radius_y

Vector Paths
~~~~~~~~~~~~

.. cpp:class:: OmegaGTE::GVectorPath2D

    A 2-D poly-line path built by appending ``GPoint2D`` points.

    .. cpp:function:: void append(const GPoint2D & pt)

        Appends a point and creates a new segment from the previous point.

    .. cpp:function:: GPoint2D & firstPt()
    .. cpp:function:: GPoint2D & lastPt()

    .. cpp:function:: float mag()

        Total arc length of the path.

    .. cpp:function:: void reset(const GPoint2D & start)

        Clears the path and sets a new starting point.

.. cpp:class:: OmegaGTE::GVectorPath3D

    The 3-D equivalent of ``GVectorPath2D``, using ``GPoint3D`` points.

.. code-block:: cpp

    // 2D path — an L-shape
    OmegaGTE::GVectorPath2D path({ 0.f, 1.f });
    path.append({ 0.f, 0.f });
    path.append({ 1.f, 0.f });

    auto params = OmegaGTE::TETriangulationParams::GraphicsPath2D(
        path, 3.f, false, false);

Matrix and Vector Types
~~~~~~~~~~~~~~~~~~~~~~~

.. cpp:class:: template<class Ty, unsigned column, unsigned row> OmegaGTE::Matrix

    A statically-sized column-major matrix. Commonly used via the type aliases:

    +--------------------+------------------------------------+
    | ``FMatrix<C, R>``  | ``float`` matrix, C columns × R rows |
    +--------------------+------------------------------------+
    | ``IMatrix<C, R>``  | ``int`` matrix                     |
    +--------------------+------------------------------------+
    | ``UMatrix<C, R>``  | ``unsigned int`` matrix            |
    +--------------------+------------------------------------+
    | ``DMatrix<C, R>``  | ``double`` matrix                  |
    +--------------------+------------------------------------+

    Single-row matrices (column vectors) have the additional aliases:

    +------------------+-----------------------------+
    | ``FVec<N>``      | ``float`` column vector     |
    +------------------+-----------------------------+
    | ``IVec<N>``      | ``int`` column vector       |
    +------------------+-----------------------------+
    | ``UVec<N>``      | ``unsigned int`` vector     |
    +------------------+-----------------------------+

    .. cpp:function:: static Matrix Create()

        Returns a zero-initialised matrix.

    .. cpp:function:: static Matrix Identity()

        Returns an identity matrix. Only valid for square matrices.

    .. cpp:function:: row_pointer_wrapper operator[](size_type col)

        Column accessor. Use ``m[col][row]`` to read/write individual elements.

    .. cpp:function:: Matrix transposed() const

        Returns the transpose of this matrix.

    .. cpp:function:: const Ty* data() const
    .. cpp:function:: Ty* data()

        Returns a pointer to the raw element array for direct GPU upload.

    **Arithmetic operators**: ``+``, ``-``, ``*`` (matrix-matrix and scalar),
    ``+=``, ``-=``, ``*=``, unary ``-``, ``==``, ``!=``.

.. code-block:: cpp

    // 4 × 4 identity matrix
    auto mvp = OmegaGTE::FMatrix<4,4>::Identity();

    // Build a simple scale matrix manually
    auto scale = OmegaGTE::FMatrix<4,4>::Create();
    scale[0][0] = 2.f;
    scale[1][1] = 2.f;
    scale[2][2] = 2.f;
    scale[3][3] = 1.f;

    // Matrix multiply
    auto result = mvp * scale;

    // Column vector (float4)
    auto color = OmegaGTE::FVec<4>::Create();
    color[0][0] = 1.f;   // r
    color[1][0] = 0.5f;  // g
    color[2][0] = 0.f;   // b
    color[3][0] = 1.f;   // a

    // Convenience helper — creates a float4 colour vector
    auto red = OmegaGTE::makeColor(1.f, 0.f, 0.f, 1.f);

    // Transpose
    auto transposed = result.transposed();

    // Raw data pointer for uploading to a GEBuffer
    const float* rawData = result.data();

FVector2D and FVector3D
~~~~~~~~~~~~~~~~~~~~~~~

.. cpp:class:: OmegaGTE::FVector2D

    A 2-D floating-point vector (``i``, ``j`` components).

    .. cpp:function:: static FVector2D FromMagnitudeAndAngle(float mag, float angle)

        Constructs a vector from a polar magnitude and angle (radians).

    .. cpp:function:: float & getI()
    .. cpp:function:: float & getJ()

    .. cpp:function:: virtual float mag()

        Euclidean length.

    .. cpp:function:: float angle()

        Angle relative to the i-axis (radians).

    .. cpp:function:: float dot(const FVector2D & vec)

        Dot product with ``vec``.

    **Operators**: ``+``, ``-``, ``+=``, ``-=``.

.. cpp:class:: OmegaGTE::FVector3D

    A 3-D floating-point vector (``i``, ``j``, ``k`` components).

    .. cpp:function:: static FVector3D FromMagnitudeAndAngles(float mag, float angle_v, float angle_h)

    .. cpp:function:: float & getK()

    .. cpp:function:: virtual float mag()

    .. cpp:function:: float angle_h()

        Horizontal angle measured from ``i`` (radians).

    .. cpp:function:: float angle_v()

        Vertical angle measured from ``i + k`` (radians).

    .. cpp:function:: float dot(const FVector3D & vec)

    .. cpp:function:: FVector3D cross(FVector3D & vec)

        Cross product.

.. code-block:: cpp

    OmegaGTE::FVector3D forward(0.f, 0.f, -1.f);
    OmegaGTE::FVector3D up(0.f, 1.f, 0.f);
    OmegaGTE::FVector3D right = forward.cross(up);

    float len = forward.mag();           // 1.0

    // Polar construction: unit vector at 45°
    auto v = OmegaGTE::FVector2D::FromMagnitudeAndAngle(1.f, 0.7854f);
