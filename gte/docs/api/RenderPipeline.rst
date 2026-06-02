===============
Render Pipeline
===============

A render pipeline is the immutable bundle of state a draw call needs:
vertex + fragment shaders, the rasteriser configuration (cull / fill / front
face / MSAA), the depth and stencil state, the blend state, and the vertex
input layout. Build a pipeline once during initialisation, reuse it across
every frame that needs it. This page covers both halves of rendering: how
you describe a pipeline, and how you drive it through a render pass.

OmegaGTE has three kinds of pipelines whose draws end up on a render
target:

* **Standard render pipeline** — vertex shader + fragment shader. The common
  case. Built via :cpp:func:`OmegaGraphicsEngine::makeRenderPipelineState`.
* **Mesh-shader render pipeline** — an optional amplification (task) stage
  and a required mesh stage produce primitives, then a fragment stage runs
  as usual. Feature-gated; built via :cpp:func:`makeMeshPipelineState`.
* **Blit pipeline** — engine-supplied full-screen-triangle vertex stage and
  a caller-supplied fragment stage that transforms one texture into another.
  See :doc:`Blitting`.

This page focuses on the first two. The pass-encoding API documented here
is shared by every render pipeline kind.

.. contents:: On this page
   :local:
   :depth: 2

RenderPipelineDescriptor
========================

.. cpp:struct:: OmegaGTE::RenderPipelineDescriptor

    .. cpp:member:: OmegaCommon::String name

        Debug label shown in GPU profilers (RenderDoc, PIX, Xcode).

    .. cpp:member:: SharedHandle<GTEShader> vertexFunc

        Vertex shader entry point from a loaded :cpp:struct:`GTEShaderLibrary`.
        See :doc:`Shaders`.

    .. cpp:member:: SharedHandle<GTEShader> fragmentFunc

        Fragment shader entry point.

    .. cpp:member:: Vector<PixelFormat> colorPixelFormats

        Pixel format of each colour attachment (multiple render targets).
        Entry ``[0]`` must match the primary render target's format; entries
        ``[1..N]`` must match the per-attachment textures listed in the
        render pass's :cpp:struct:`ColorAttachment` array. Up to eight
        attachments. Default ``{ RGBA8Unorm }``.

    .. cpp:member:: PrimitiveTopologyCategory primitiveTopologyCategory

        Topology class baked into the pipeline state. One of ``Triangle``
        (default), ``Line``, or ``Point``. The concrete topology at draw
        time (triangle list / strip, line list / strip, point) is chosen by
        the ``PolygonType`` passed to ``drawPolygons``, but it must belong
        to this category. This field is load-bearing on D3D12, which bakes
        the category into the PSO; Metal and Vulkan derive the topology
        from the draw call directly.

    .. cpp:member:: unsigned rasterSampleCount

        MSAA sample count. ``0`` or ``1`` disables MSAA. Must be
        ``≤ GTEDeviceFeatures::maxMSAASamples`` and the render target's
        textures must have a matching sample count.

    .. cpp:member:: RasterCullMode cullMode

        +------------------+---------------------------------------------------+
        | ``None``         | Draw all triangles regardless of winding order.   |
        +------------------+---------------------------------------------------+
        | ``Front``        | Discard front-facing triangles.                   |
        +------------------+---------------------------------------------------+
        | ``Back``         | Discard back-facing triangles (default for 3D).   |
        +------------------+---------------------------------------------------+

    .. cpp:member:: TriangleFillMode triangleFillMode

        ``Solid`` (default) or ``Wireframe``. Wireframe requires
        ``GTEDEVICE_FEATURE_FILL_MODE_NON_SOLID``.

    .. cpp:member:: GTEPolygonFrontFaceRotation polygonFrontFaceRotation

        ``Clockwise`` (default) or ``CounterClockwise`` — controls which
        winding order counts as "front-facing".

    .. cpp:member:: VertexInputDescriptor vertexInputDescriptor

        Describes how vertex buffer bytes map to vertex shader inputs. See
        *Vertex input layout* below. Leave empty to fall back to the
        shader's reflected inputs (legacy behaviour).

    .. cpp:member:: Vector<BlendDescriptor> colorBlendDescriptors

        Per-attachment blend state. Index ``i`` configures colour
        attachment ``i``. Empty means opaque writes (no blending). See
        *Blend state* below.

    .. cpp:member:: DepthStencilDesc depthAndStencilDesc

        Depth + stencil testing. See *Depth and stencil* below.

Vertex input layout
===================

The vertex input descriptor tells the rasteriser how to walk vertex buffer
bytes into vertex shader inputs. Leaving it empty falls back to a "use
whatever the shader's reflected inputs say" path that works for simple
cases. The moment you have multiple vertex buffer slots (separated
positions and UVs, interleaved-but-stepped-per-instance transforms, etc.)
you need to set it explicitly.

.. cpp:enum-class:: OmegaGTE::VertexFormat

    Vertex attribute element types, with their byte size:

    .. list-table::
       :widths: 45 55
       :header-rows: 0

       * - ``Float``
         - 4 bytes
       * - ``Float2``
         - 8 bytes
       * - ``Float3``
         - 12 bytes
       * - ``Float4``
         - 16 bytes
       * - ``Int`` / ``Int2`` / ``Int3`` / ``Int4``
         - 4 / 8 / 12 / 16 bytes
       * - ``UInt`` / ``UInt2`` / ``UInt3`` / ``UInt4``
         - 4 / 8 / 12 / 16 bytes
       * - ``UNorm8x4``
         - 4 bytes (normalised to ``[0, 1]``)
       * - ``SNorm8x4``
         - 4 bytes (normalised to ``[-1, 1]``)
       * - ``UShort2``
         - 4 bytes (raw 16-bit unsigned)
       * - ``UShort4``
         - 8 bytes
       * - ``Half2``
         - 4 bytes (16-bit float)
       * - ``Half4``
         - 8 bytes

.. cpp:enum-class:: OmegaGTE::VertexStepFunction

    How the input rate advances:

    +------------------+----------------------------------------------------+
    | ``PerVertex``    | Advance one element per vertex.                    |
    +------------------+----------------------------------------------------+
    | ``PerInstance``  | Advance one element every ``stepRate`` instances.  |
    +------------------+----------------------------------------------------+

.. cpp:struct:: OmegaGTE::VertexBufferLayout

    One vertex buffer binding slot.

    .. cpp:member:: unsigned stride

        Byte distance from one element to the next in this buffer.

    .. cpp:member:: VertexStepFunction stepFunction

        ``PerVertex`` (default) or ``PerInstance``.

    .. cpp:member:: unsigned stepRate

        ``1`` for per-vertex; for per-instance, the instance divisor
        (how many instances reuse the same element).

.. cpp:struct:: OmegaGTE::VertexAttribute

    One attribute exposed to the vertex shader.

    .. cpp:member:: unsigned bufferIndex

        Which :cpp:struct:`VertexBufferLayout` slot this attribute reads
        from.

    .. cpp:member:: unsigned offset

        Byte offset within one element of that buffer.

    .. cpp:member:: VertexFormat format

        Channel count + precision. Default ``Float4``.

    .. cpp:member:: unsigned shaderLocation

        OmegaSL input location the attribute binds to.

.. cpp:struct:: OmegaGTE::VertexInputDescriptor

    .. cpp:member:: Vector<VertexBufferLayout> bufferLayouts
    .. cpp:member:: Vector<VertexAttribute> attributes

        The complete layout. Buffer layouts are indexed by
        :cpp:member:`VertexAttribute::bufferIndex`.

.. code-block:: cpp

    // Interleaved (pos, uv) in slot 0; per-instance transforms in slot 1.
    OmegaGTE::VertexInputDescriptor vi;

    OmegaGTE::VertexBufferLayout perVert;
    perVert.stride       = sizeof(float) * 6;          // float4 + float2
    perVert.stepFunction = OmegaGTE::VertexStepFunction::PerVertex;
    vi.bufferLayouts.push_back(perVert);

    OmegaGTE::VertexBufferLayout perInst;
    perInst.stride       = sizeof(float) * 16;         // float4x4
    perInst.stepFunction = OmegaGTE::VertexStepFunction::PerInstance;
    perInst.stepRate     = 1;
    vi.bufferLayouts.push_back(perInst);

    OmegaGTE::VertexAttribute pos;
    pos.bufferIndex    = 0;
    pos.offset         = 0;
    pos.format         = OmegaGTE::VertexFormat::Float4;
    pos.shaderLocation = 0;
    vi.attributes.push_back(pos);

    OmegaGTE::VertexAttribute uv;
    uv.bufferIndex    = 0;
    uv.offset         = sizeof(float) * 4;
    uv.format         = OmegaGTE::VertexFormat::Float2;
    uv.shaderLocation = 1;
    vi.attributes.push_back(uv);

    // …attributes for the four float4 columns of the per-instance matrix…

    pipeDesc.vertexInputDescriptor = vi;

Blend state
===========

A blend descriptor tells the output-merger how to combine the fragment
shader's output with the existing colour attachment value. Provide one
descriptor per colour attachment. Without any blend descriptor an
attachment writes opaquely (no blending). To enable
``GTEDEVICE_FEATURE_INDEPENDENT_BLEND`` differences across attachments,
populate them differently per index; without that feature, every entry
must match.

.. cpp:enum-class:: OmegaGTE::BlendFactor

    Source / destination factor multiplied into the colour or alpha term:

    .. list-table::
       :widths: 45 55
       :header-rows: 0

       * - ``Zero``
         - 0
       * - ``One``
         - 1
       * - ``SrcColor`` / ``InvSrcColor``
         - C\ :sub:`src` / 1 − C\ :sub:`src`
       * - ``SrcAlpha`` / ``InvSrcAlpha``
         - A\ :sub:`src` / 1 − A\ :sub:`src`
       * - ``DestColor`` / ``InvDestColor``
         - C\ :sub:`dst` / 1 − C\ :sub:`dst`
       * - ``DestAlpha`` / ``InvDestAlpha``
         - A\ :sub:`dst` / 1 − A\ :sub:`dst`
       * - ``SrcAlphaSaturated``
         - min(A\ :sub:`src`, 1 − A\ :sub:`dst`)
       * - ``Src1Color`` / ``InvSrc1Color`` / ``Src1Alpha`` / ``InvSrc1Alpha``
         - Dual-source — requires ``GTEDEVICE_FEATURE_DUAL_SOURCE_BLENDING``.

.. cpp:enum-class:: OmegaGTE::BlendOperation

    Final equation between the (source × srcFactor) and (dest × destFactor)
    terms:

    +-----------------------+----------------------+
    | ``Add``               | a + b                |
    +-----------------------+----------------------+
    | ``Subtract``          | a − b                |
    +-----------------------+----------------------+
    | ``ReverseSubtract``   | b − a                |
    +-----------------------+----------------------+
    | ``Min``               | min(a, b)            |
    +-----------------------+----------------------+
    | ``Max``               | max(a, b)            |
    +-----------------------+----------------------+

.. cpp:enum:: OmegaGTE::ColorWriteMask

    OR-together write enables: ``ColorWriteRed``, ``ColorWriteGreen``,
    ``ColorWriteBlue``, ``ColorWriteAlpha``, ``ColorWriteNone``,
    ``ColorWriteAll``. Use ``ColorWriteNone`` to render passes that touch
    depth only.

.. cpp:struct:: OmegaGTE::BlendDescriptor

    .. cpp:member:: bool blendEnabled

        Master enable. Default ``false`` — opaque write.

    .. cpp:member:: BlendFactor srcColorFactor
    .. cpp:member:: BlendFactor destColorFactor
    .. cpp:member:: BlendOperation colorOp

        RGB-channel equation. Default sets up standard alpha blending:
        ``SrcAlpha * srcRGB + InvSrcAlpha * dstRGB``.

    .. cpp:member:: BlendFactor srcAlphaFactor
    .. cpp:member:: BlendFactor destAlphaFactor
    .. cpp:member:: BlendOperation alphaOp

        Alpha-channel equation, controlled independently of RGB.

    .. cpp:member:: uint8_t writeMask

        Per-channel write mask. Default ``ColorWriteAll``.

.. code-block:: cpp

    // Standard premultiplied alpha blending on attachment 0.
    OmegaGTE::BlendDescriptor blend;
    blend.blendEnabled    = true;
    blend.srcColorFactor  = OmegaGTE::BlendFactor::One;
    blend.destColorFactor = OmegaGTE::BlendFactor::InvSrcAlpha;
    blend.colorOp         = OmegaGTE::BlendOperation::Add;
    blend.srcAlphaFactor  = OmegaGTE::BlendFactor::One;
    blend.destAlphaFactor = OmegaGTE::BlendFactor::InvSrcAlpha;
    blend.alphaOp         = OmegaGTE::BlendOperation::Add;

    pipeDesc.colorBlendDescriptors = { blend };

Depth and stencil
=================

.. cpp:struct:: OmegaGTE::RenderPipelineDescriptor::DepthStencilDesc

    Lives at ``pipeDesc.depthAndStencilDesc``.

    .. cpp:member:: bool enableDepth

        Enable depth testing. Default ``false``.

    .. cpp:member:: bool enableStencil

        Enable stencil testing. Default ``false``.

    .. cpp:member:: DepthWriteAmount writeAmount

        ``All`` (default) writes depth; ``Zero`` prevents depth writes
        (useful for transparency passes that should test but not occlude).

    .. cpp:member:: CompareFunc depthOperation

        Depth comparison. ``Less`` (default) — passes fragments closer to
        the camera. Other options: ``LessEqual``, ``Greater``,
        ``GreaterEqual``.

    .. cpp:member:: float depthBias
    .. cpp:member:: float slopeScale
    .. cpp:member:: float depthClamp

        Depth-bias parameters used for shadow mapping and to mitigate
        z-fighting on coplanar geometry. ``depthClamp`` requires
        ``GTEDEVICE_FEATURE_DEPTH_BIAS_CLAMP``.

    .. cpp:member:: unsigned stencilReadMask
    .. cpp:member:: unsigned stencilWriteMask

        Masks ANDed with the stencil buffer before read / write.

    .. cpp:struct:: StencilDesc

        Per-face stencil operation set:

        .. cpp:member:: StencilOperation stencilFail
        .. cpp:member:: StencilOperation depthFail
        .. cpp:member:: StencilOperation pass

            Operations executed when stencil fails, when stencil passes
            but depth fails, and when both pass. Options: ``Retain``,
            ``Zero``, ``Replace``, ``IncrementWrap``, ``DecrementWrap``.

        .. cpp:member:: CompareFunc func

            Stencil test comparison function.

    .. cpp:member:: StencilDesc frontFaceStencil
    .. cpp:member:: StencilDesc backFaceStencil

        Independent stencil state per face winding.

Building the pipeline
=====================

.. code-block:: cpp

    auto shaderLib = gte.graphicsEngine->loadShaderLibrary("shaders.omegasllib");

    OmegaGTE::RenderPipelineDescriptor pipeDesc;
    pipeDesc.name              = "ForwardOpaque";
    pipeDesc.vertexFunc        = shaderLib->shaders["forwardVS"];
    pipeDesc.fragmentFunc      = shaderLib->shaders["forwardFS"];
    pipeDesc.colorPixelFormats = { OmegaGTE::PixelFormat::BGRA8Unorm };
    pipeDesc.cullMode          = OmegaGTE::RasterCullMode::Back;

    pipeDesc.depthAndStencilDesc.enableDepth    = true;
    pipeDesc.depthAndStencilDesc.depthOperation = OmegaGTE::CompareFunc::Less;
    pipeDesc.depthAndStencilDesc.writeAmount    = OmegaGTE::DepthWriteAmount::All;

    auto pipeline = gte.graphicsEngine->makeRenderPipelineState(pipeDesc);
    if (!pipeline) {
        // makeRenderPipelineState returns null when an input shader is an
        // unsupported sentinel — read the diagnostic on the shader handle.
    }

Mesh-shader pipelines
=====================

A mesh-shader pipeline replaces the vertex-input + vertex-stage half with a
mesh shader and (optionally) an amplification (task) shader. The geometry
side emits primitives directly; there is no vertex-buffer pull and no
``primitiveTopologyCategory``. The fragment stage and the rasteriser
configuration are identical to a standard render pipeline.

Feature-gated behind ``GTEDEVICE_FEATURE_MESH_SHADER``: on devices that do
not advertise it, :cpp:func:`makeMeshPipelineState` returns ``nullptr`` and
writes a diagnostic to ``stderr``. Dispatch with
:cpp:func:`drawMeshTasks` (below) — not ``drawPolygons``.

.. cpp:struct:: OmegaGTE::MeshPipelineDescriptor

    .. cpp:member:: OmegaCommon::String name

    .. cpp:member:: SharedHandle<GTEShader> amplificationFunc

        Optional task shader that dispatches mesh threadgroups. ``nullptr``
        is valid — every backend supports running a mesh pipeline without
        a task stage.

    .. cpp:member:: SharedHandle<GTEShader> meshFunc

        Required mesh shader. Compiled from an OmegaSL
        ``mesh(max_vertices=..., max_primitives=..., topology=...)`` entry.

    .. cpp:member:: SharedHandle<GTEShader> fragmentFunc

        Required fragment shader.

    .. cpp:member:: Vector<PixelFormat> colorPixelFormats
    .. cpp:member:: unsigned rasterSampleCount
    .. cpp:member:: RasterCullMode cullMode
    .. cpp:member:: TriangleFillMode triangleFillMode
    .. cpp:member:: GTEPolygonFrontFaceRotation polygonFrontFaceRotation
    .. cpp:member:: Vector<BlendDescriptor> colorBlendDescriptors
    .. cpp:member:: RenderPipelineDescriptor::DepthStencilDesc depthAndStencilDesc

        Same shape as in :cpp:struct:`RenderPipelineDescriptor`.

The returned handle is :cpp:type:`GERenderPipelineState` — every backend
models mesh PSOs as a render-pipeline variant on the public side, so you
bind it with ``setRenderPipelineState`` exactly like a standard pipeline.

Render pass encoding
====================

Render passes are encoded on a :cpp:class:`OmegaGTE::GECommandBuffer`
obtained from a command queue (see :doc:`GPUSubmission`). One command
buffer can record several passes; a render pass is opened with
``startRenderPass`` and closed with ``finishRenderPass``.

Render pass descriptor
~~~~~~~~~~~~~~~~~~~~~~

.. cpp:struct:: OmegaGTE::GERenderPassDescriptor

    .. cpp:member:: GENativeRenderTarget *nRenderTarget
    .. cpp:member:: GETextureRenderTarget *tRenderTarget

        The target the pass writes into. Set exactly one. When
        ``colorAttachments[0].texture`` is null, the pass writes
        to whichever of these targets you set.

    .. cpp:member:: Vector<ColorAttachment> colorAttachments

        One :cpp:struct:`ColorAttachment` per colour bind. Index ``[0]``
        is the primary; index ``[1..N]`` requires a per-attachment
        texture.

    .. cpp:member:: DepthStencilAttachment depthStencilAttachment

        Initial depth/stencil state for the pass.

    .. cpp:member:: bool multisampleResolve
    .. cpp:member:: MultisampleResolveDesc resolveDesc

        Set ``multisampleResolve = true`` and populate ``resolveDesc`` to
        resolve a multisampled source texture into a 1-sample destination
        at the end of the pass.

.. cpp:struct:: OmegaGTE::GERenderPassDescriptor::ColorAttachment

    .. cpp:type:: LoadAction

        .. list-table::
           :widths: 30 70
           :header-rows: 0

           * - ``Load``
             - Keep the existing contents.
           * - ``LoadPreserve``
             - Load and preserve (backend hint for explicit preservation across passes).
           * - ``Clear``
             - Fill with ``clearColor`` before rendering.
           * - ``Discard``
             - Discard the previous contents (fastest).

    .. cpp:member:: LoadAction loadAction
    .. cpp:member:: ClearColor clearColor
    .. cpp:member:: SharedHandle<GETexture> texture

        Per-attachment texture; null for attachment 0 to fall back to the
        pass's render target.

.. cpp:struct:: OmegaGTE::GERenderPassDescriptor::DepthStencilAttachment

    .. cpp:member:: bool disabled

        Default ``true``. Set to ``false`` to activate depth/stencil.

    .. cpp:member:: LoadAction depthloadAction
    .. cpp:member:: LoadAction stencilLoadAction
    .. cpp:member:: float clearDepth
    .. cpp:member:: unsigned clearStencil

        Initial state for the depth and stencil buffers. ``clearDepth``
        defaults to ``1.0`` (far plane); ``clearStencil`` defaults to ``0``.

.. cpp:struct:: OmegaGTE::GERenderPassDescriptor::MultisampleResolveDesc

    .. cpp:member:: SharedHandle<GETexture> multiSampleTextureSrc
    .. cpp:member:: unsigned level
    .. cpp:member:: unsigned slice
    .. cpp:member:: unsigned depth

Encoding commands
~~~~~~~~~~~~~~~~~

The methods below all live on :cpp:class:`OmegaGTE::GECommandBuffer`. The
order they must appear in inside a pass is roughly: open → set pipeline →
bind resources → set dynamic state → draw → close.

.. cpp:namespace-push:: OmegaGTE::GECommandBuffer

**Opening and closing**

.. cpp:function:: void startRenderPass(const GERenderPassDescriptor & desc)
.. cpp:function:: void finishRenderPass()

**Pipeline + vertex / index buffers**

.. cpp:function:: void setRenderPipelineState(SharedHandle<GERenderPipelineState> & pipelineState)

    Bind the pipeline that controls subsequent draws.

.. cpp:function:: void setVertexBuffer(SharedHandle<GEBuffer> & buffer)

    Bind a buffer to the vertex input slot at vertex-input slot 0.
    Use multi-slot vertex inputs via the
    :cpp:struct:`VertexInputDescriptor` you set on the pipeline.

.. cpp:function:: void setIndexBuffer(SharedHandle<GEBuffer> & buffer, IndexType indexType)

    Bind an index buffer for subsequent ``drawIndexedPolygons*`` calls.
    ``IndexType`` is ``UInt16`` or ``UInt32``.

**Resource binds**

.. cpp:function:: void bindResourceAtVertexShader(SharedHandle<GEBuffer> & buffer, unsigned id)
.. cpp:function:: void bindResourceAtVertexShader(SharedHandle<GETexture> & texture, unsigned id, const TextureSwizzle & swizzle = TextureSwizzle::identity())
.. cpp:function:: void bindResourceAtVertexShader(SharedHandle<GESamplerState> & sampler, unsigned id)

    Bind a buffer, texture (with optional per-bind swizzle), or sampler
    to the vertex shader at OmegaSL register ``id``.

.. cpp:function:: void bindResourceAtFragmentShader(SharedHandle<GEBuffer> & buffer, unsigned id)
.. cpp:function:: void bindResourceAtFragmentShader(SharedHandle<GETexture> & texture, unsigned id, const TextureSwizzle & swizzle = TextureSwizzle::identity())
.. cpp:function:: void bindResourceAtFragmentShader(SharedHandle<GESamplerState> & sampler, unsigned id)

    Same, fragment side.

**Push constants**

.. cpp:function:: void setRenderConstants(const void *data, unsigned size, unsigned offset = 0)

    Update the bound pipeline's push-constant block (OmegaSL
    ``constant<T>``). ≤ 128 bytes portable; updates without a buffer
    allocation. Bytes must already be in the std-layout the shader
    expects (std430 for the Vulkan ``push_constant`` form). Metal
    supports only ``offset == 0``. Must be called inside a render pass
    with a pipeline bound.

**Dynamic state**

.. cpp:function:: void setViewports(std::vector<GEViewport> viewports)
.. cpp:function:: void setScissorRects(std::vector<GEScissorRect> scissorRects)
.. cpp:function:: void setStencilRef(unsigned ref)

    Per-draw dynamic state. Multiple viewports require the pipeline's
    primitive category and the device to support it.

**Draws**

.. cpp:function:: void drawPolygons(PolygonType polygonType, unsigned vertexCount, size_t startIdx)

    Non-indexed draw.

.. cpp:function:: void drawIndexedPolygons(PolygonType polygonType, unsigned indexCount, size_t startIndex, int baseVertex)

    Indexed draw. Requires :cpp:func:`setIndexBuffer` to have been
    called first.

.. cpp:function:: void drawPolygonsInstanced(PolygonType polygonType, unsigned vertexCount, size_t startIdx, unsigned instanceCount, unsigned firstInstance)
.. cpp:function:: void drawIndexedPolygonsInstanced(PolygonType polygonType, unsigned indexCount, size_t startIndex, int baseVertex, unsigned instanceCount, unsigned firstInstance)

    Instanced variants. ``firstInstance != 0`` requires
    ``GTEDEVICE_FEATURE_DRAW_INDIRECT_FIRST_INSTANCE``.

.. cpp:function:: void drawPolygonsIndirect(PolygonType polygonType, SharedHandle<GEBuffer> & argumentBuffer, size_t argumentBufferOffset)
.. cpp:function:: void drawIndexedPolygonsIndirect(PolygonType polygonType, SharedHandle<GEBuffer> & argumentBuffer, size_t argumentBufferOffset)

    GPU-driven draws — read draw arguments from a buffer the GPU wrote.
    The buffer must hold a :cpp:struct:`GEDrawIndirectCommand` or
    :cpp:struct:`GEDrawIndexedIndirectCommand` at
    ``argumentBufferOffset``. Multi-draw requires
    ``GTEDEVICE_FEATURE_MULTI_DRAW_INDIRECT``.

.. cpp:function:: void drawMeshTasks(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)

    Dispatch a mesh-shader pipeline. Must be called inside a render
    pass with a mesh PSO bound. Per-meshlet workgroup dimensions come
    from the bound mesh shader. Feature-gated behind
    ``GTEDEVICE_FEATURE_MESH_SHADER``.

**High-level mesh helpers** — bind a :doc:`mesh asset <Triangulation>` in
one call:

.. cpp:function:: void bindMesh(SharedHandle<GEMesh> & mesh, unsigned vertexSlot = 0)
.. cpp:function:: void drawMesh(SharedHandle<GEMesh> & mesh, unsigned vertexSlot = 0)

.. cpp:namespace-pop::

Indirect command argument structs
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The arguments to :cpp:func:`drawPolygonsIndirect` and friends live in a
buffer with these layouts (matching the underlying D3D12 / Vulkan / Metal
structures byte-for-byte):

.. cpp:struct:: OmegaGTE::GEDrawIndirectCommand

    .. cpp:member:: std::uint32_t vertexCount
    .. cpp:member:: std::uint32_t instanceCount
    .. cpp:member:: std::uint32_t firstVertex
    .. cpp:member:: std::uint32_t firstInstance

.. cpp:struct:: OmegaGTE::GEDrawIndexedIndirectCommand

    .. cpp:member:: std::uint32_t indexCount
    .. cpp:member:: std::uint32_t instanceCount
    .. cpp:member:: std::uint32_t firstIndex
    .. cpp:member:: std::int32_t baseVertex
    .. cpp:member:: std::uint32_t firstInstance

GEViewport and GEScissorRect
============================

.. cpp:struct:: OmegaGTE::GEViewport

    The 3-D viewport mapped to the render target.

    .. cpp:member:: float x
    .. cpp:member:: float y

        Upper-left corner in pixel coordinates.

    .. cpp:member:: float width
    .. cpp:member:: float height

        Viewport dimensions.

    .. cpp:member:: float nearDepth
    .. cpp:member:: float farDepth

        Depth range, typically ``0.0`` (near) and ``1.0`` (far).

.. cpp:struct:: OmegaGTE::GEScissorRect

    A 2-D rectangle that clips the viewport. Fragments outside the
    rectangle are discarded before the fragment shader runs.

    .. cpp:member:: float x
    .. cpp:member:: float y
    .. cpp:member:: float width
    .. cpp:member:: float height

Complete render-loop example
============================

.. code-block:: cpp

    // Once at startup:
    auto queue    = gte.graphicsEngine->makeCommandQueue(3);
    auto nativeRT = gte.graphicsEngine->makeNativeRenderTarget(rtDesc, queue);
    auto pipeline = gte.graphicsEngine->makeRenderPipelineState(pipeDesc);

    // Each frame:
    auto cmdBuf = queue->getAvailableBuffer();
    cmdBuf->setName("FrameCommands");

    // Pass descriptor: clear to dark grey, clear depth to 1.0.
    OmegaGTE::GERenderPassDescriptor passDesc;
    passDesc.nRenderTarget = nativeRT.get();

    OmegaGTE::GERenderPassDescriptor::ColorAttachment color(
        { 0.1f, 0.1f, 0.1f, 1.0f },
        OmegaGTE::GERenderPassDescriptor::ColorAttachment::Clear
    );
    passDesc.colorAttachments.push_back(color);

    passDesc.depthStencilAttachment.disabled        = false;
    passDesc.depthStencilAttachment.depthloadAction = OmegaGTE::GERenderPassDescriptor::DepthStencilAttachment::Clear;
    passDesc.depthStencilAttachment.clearDepth      = 1.0f;

    cmdBuf->startRenderPass(passDesc);
    cmdBuf->setRenderPipelineState(pipeline);

    cmdBuf->setViewports({{ 0.f, 0.f, (float)width, (float)height, 0.f, 1.f }});

    cmdBuf->setVertexBuffer(vertexBuffer);
    cmdBuf->setIndexBuffer(indexBuffer, OmegaGTE::GECommandBuffer::IndexType::UInt32);
    cmdBuf->bindResourceAtFragmentShader(diffuseTex,    0);
    cmdBuf->bindResourceAtFragmentShader(linearSampler, 1);

    // Per-draw constants (e.g. a model matrix) via push constants.
    cmdBuf->setRenderConstants(&modelMatrix, sizeof(modelMatrix));

    cmdBuf->drawIndexedPolygons(
        OmegaGTE::GECommandBuffer::Triangle,
        indexCount,
        0,        // startIndex
        0         // baseVertex
    );

    cmdBuf->finishRenderPass();

    queue->submitCommandBuffer(cmdBuf);
    queue->commitToGPU();
    nativeRT->present();

    cmdBuf->reset();   // ready for the next frame

Common pitfalls
===============

* **Topology mismatch.** ``Triangle`` / ``TriangleStrip`` requires the
  pipeline's ``primitiveTopologyCategory`` to be ``Triangle``; ``Line`` /
  ``LineStrip`` requires ``Line``; ``Point`` requires ``Point``. The
  category is baked into the PSO on D3D12 — a mismatched draw is rejected.
* **Forgetting to bind the vertex / index buffer.** ``drawPolygons*``
  reads vertex data from the buffer last bound via ``setVertexBuffer``;
  indexed draws additionally require ``setIndexBuffer``.
* **Push constants without a bound pipeline.** ``setRenderConstants`` must
  be called after ``setRenderPipelineState`` and inside a render pass.
* **Sample-count disagreement.** ``rasterSampleCount`` on the pipeline must
  match the sample count of the render target's textures and the
  attachment's sample count. Mismatches fail at pipeline build.
* **Per-attachment blend differences without the feature bit.** Each entry
  of ``colorBlendDescriptors`` must match unless the device advertises
  ``GTEDEVICE_FEATURE_INDEPENDENT_BLEND``.
* **Indirect-draw arguments in the wrong struct shape.** The argument
  buffer must hold a :cpp:struct:`GEDrawIndirectCommand` (or its indexed
  cousin) at ``argumentBufferOffset`` — exact field order matters.
