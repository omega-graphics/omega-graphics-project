========
Blitting
========

"Blitting" in OmegaGTE covers two related but distinct concepts: a **blit
pass**, which copies bytes between GPU resources without involving any
shader, and a **blit pipeline**, which runs a caller-supplied fragment
shader as a full-screen pass that transforms one texture into another.
Both flavours sit on the command-buffer surface.

* **Blit pass** — exact bit copy. Use for shadow-atlas packing, swap-chain
  back-buffer copies, GPU-side buffer-to-texture / texture-to-buffer
  streaming, and mipmap generation. Fast (uses the device's DMA path on
  most hardware), no shader execution.
* **Blit pipeline** — programmable colour transform. Use for tone mapping,
  format conversion, sharpening, downsampling with a custom filter — any
  case where you need to *modify* texels while copying, not just move
  them. Internally runs a fragment shader covering the destination
  extent, with the source bound as an input.

.. contents:: On this page
   :local:
   :depth: 2

Blit pass
=========

A blit pass is bracketed by ``startBlitPass`` / ``finishBlitPass`` on a
command buffer (see :doc:`GPUSubmission`). Between them you encode one or
more byte-copy operations.

.. cpp:function:: void GECommandBuffer::startBlitPass()
.. cpp:function:: void GECommandBuffer::finishBlitPass()

    Open and close the blit pass.

.. cpp:struct:: OmegaGTE::GEBlitPassDescriptor

    Currently empty — pass a default-constructed value if a future
    revision requires one.

The copy operations available inside the pass:

.. cpp:function:: void GECommandBuffer::copyTextureToTexture(\
    SharedHandle<GETexture> & src, SharedHandle<GETexture> & dest)

    Copy the entire ``src`` into ``dest``. The two textures must have
    matching dimensions and a compatible pixel format.

.. cpp:function:: void GECommandBuffer::copyTextureToTexture(\
    SharedHandle<GETexture> & src, SharedHandle<GETexture> & dest,\
    const TextureRegion & region, const GPoint3D & destCoord)

    Copy a sub-``region`` of ``src`` into ``dest`` at origin
    ``destCoord``. Use for atlas packing and partial updates. See
    :doc:`Textures` for ``TextureRegion`` and :doc:`CPUMath` for
    ``GPoint3D``.

.. cpp:function:: void GECommandBuffer::copyBufferToBuffer(\
    SharedHandle<GEBuffer> & src, SharedHandle<GEBuffer> & dest,\
    size_t size = 0, size_t srcOffset = 0, size_t destOffset = 0)

    GPU-to-GPU buffer copy. ``size = 0`` means "copy from ``srcOffset`` to
    the end of ``src``". Also available outside a blit pass — see
    :doc:`GPUSubmission`.

.. cpp:function:: void GECommandBuffer::copyBufferToTexture(\
    SharedHandle<GEBuffer> & src, SharedHandle<GETexture> & dest,\
    size_t bytesPerRow, size_t bytesPerImage,\
    const TextureRegion & destRegion, size_t srcBufferOffset = 0)

    Stream a packed buffer's bytes into a texture region. Use when you
    have a CPU-side ``Upload`` buffer staging texture data — the buffer
    is the bridge between CPU writes and the device-local texture.

.. cpp:function:: void GECommandBuffer::copyTextureToBuffer(\
    SharedHandle<GETexture> & src, SharedHandle<GEBuffer> & dest,\
    size_t bytesPerRow, size_t bytesPerImage,\
    const TextureRegion & srcRegion, size_t destBufferOffset = 0)

    GPU readback of a texture region into a buffer. Pair with a
    ``Readback`` buffer to inspect pixels on the CPU.

.. cpp:function:: void GECommandBuffer::generateMipmaps(SharedHandle<GETexture> & texture)

    Fill mip levels 1..N of ``texture`` by downsampling from mip 0. The
    backend picks the filter it deems appropriate (a 2×2 box filter on
    most hardware). The texture must have been allocated with
    ``mipLevels > 1``.

.. code-block:: cpp

    // Pack a shadow map into column 1 of a 2048-wide atlas.
    OmegaGTE::TextureRegion srcRegion{ 0, 0, 0, 1024, 1024, 1 };
    OmegaGTE::GPoint3D      destOrigin{ 1024.f, 0.f, 0.f };

    auto cmd = queue->getAvailableBuffer();
    cmd->startBlitPass();
    cmd->copyTextureToTexture(shadowMapTex, atlasTexture, srcRegion, destOrigin);
    cmd->finishBlitPass();
    queue->submitCommandBuffer(cmd);
    queue->commitToGPU();

    // Generate mip chain for an uploaded asset.
    cmd = queue->getAvailableBuffer();
    cmd->startBlitPass();
    cmd->generateMipmaps(diffuseTex);
    cmd->finishBlitPass();
    queue->submitCommandBuffer(cmd);
    queue->commitToGPU();

Blit pipeline
=============

A blit pipeline wraps a caller-supplied fragment shader. The vertex stage
is supplied by the engine — a built-in full-screen triangle that emits
position + UV. At dispatch time, the engine opens a transient render pass
on the destination texture, binds the source as a fragment-shader input
at slot 0, and issues a 3-vertex draw covering the destination. The
fragment shader you wrote runs once per destination texel.

The fragment shader has a fixed input contract: it takes a single
``OmegaGTEBlitVertexData``-shaped struct with ``float4 pos : Position``
and ``float2 uv : TexCoord`` members (any struct name is fine — the
contract is on the field layout). It must declare its own
``static sampler2d`` for sampling the source (OmegaSL bakes static
samplers into the shader source; there is no runtime sampler binding for
blit fragments today).

.. cpp:struct:: OmegaGTE::BlitPipelineDescriptor

    .. cpp:member:: OmegaCommon::String name

        Debug label.

    .. cpp:member:: SharedHandle<GTEShader> fragmentFunc

        The user-supplied fragment shader. Must consume the
        ``OmegaGTEBlitVertexData`` rasteriser output and write to a
        single colour output (``fragment float4 ...``).

    .. cpp:member:: PixelFormat srcPixelFormat

        Source texture's pixel format. Currently advisory — used for
        validation / documentation, not consumed by pipeline creation
        (textures advertise their own format at bind time). Default
        ``RGBA8Unorm``.

    .. cpp:member:: PixelFormat destPixelFormat

        Destination texture's pixel format. Drives the underlying
        render-pipeline's colour-attachment format. Default
        ``RGBA8Unorm``.

    .. cpp:member:: unsigned srcSampleCount

        Sample count of the source texture (for MSAA-resolve blits).
        Default ``1``. Currently advisory.

.. cpp:type:: OmegaGTE::GEBlitPipelineState

    Opaque handle returned by :cpp:func:`makeBlitPipelineState`.

The pipeline is dispatched by ``blitWithPipeline`` — a single call that
opens its own render pass on the destination, so it **must not** be called
inside an existing ``startRenderPass`` / ``startBlitPass`` /
``startComputePass`` scope.

.. cpp:function:: void GECommandBuffer::blitWithPipeline(\
    SharedHandle<GEBlitPipelineState> & pipelineState,\
    SharedHandle<GETexture> & src, SharedHandle<GETexture> & dest)

    Full-extent blit — the destination's entire mip-0 layer is written.

.. cpp:function:: void GECommandBuffer::blitWithPipeline(\
    SharedHandle<GEBlitPipelineState> & pipelineState,\
    SharedHandle<GETexture> & src, SharedHandle<GETexture> & dest,\
    const TextureRegion & srcRegion, const TextureRegion & destRegion)

    Subregion blit. ``destRegion`` drives the viewport and scissor;
    ``srcRegion`` is currently advisory (the fragment shader sees the
    full source UVs).

.. code-block:: cpp

    OmegaGTE::BlitPipelineDescriptor blitDesc;
    blitDesc.name            = "ACESToneMap";
    blitDesc.fragmentFunc    = shaderLib->shaders["toneMapACES"];
    blitDesc.srcPixelFormat  = OmegaGTE::PixelFormat::RGBA16Unorm;
    blitDesc.destPixelFormat = OmegaGTE::PixelFormat::BGRA8Unorm_SRGB;
    auto toneMap = gte.graphicsEngine->makeBlitPipelineState(blitDesc);

    auto cmd = queue->getAvailableBuffer();
    cmd->blitWithPipeline(toneMap, hdrTexture, ldrTexture);
    queue->submitCommandBuffer(cmd);
    queue->commitToGPU();

Common pitfalls
===============

* **Mismatched dimensions on a plain texture-to-texture copy.** The pass
  flavour requires source and destination dimensions to agree (or the
  region to fit). For *resizing* copies, use a blit pipeline.
* **Calling** ``blitWithPipeline`` **inside an existing pass.** It owns
  its own render pass; call it between passes, not inside one.
* **Generating mipmaps on a texture with ``mipLevels == 1``.** There is
  nothing to fill. Set ``mipLevels`` on the descriptor at creation if you
  intend to mip the texture.
* **Forgetting the static sampler in a blit fragment shader.** The blit
  fragment is responsible for declaring a ``static sampler2d`` to read
  the source — there is no runtime sampler bound for it today.
* **Different ``srcPixelFormat`` and source texture format.** The field
  is advisory but inconsistencies are confusing — keep the descriptor
  honest so a future revision that consumes the field still works.
