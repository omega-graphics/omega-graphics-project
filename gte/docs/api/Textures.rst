========
Textures
========

A texture is a GPU-resident image: a rectangular block of texels that
shaders sample, render targets write into, and compute passes read or
modify. OmegaGTE textures unify nine *kinds* (1D, 2D, 3D, array variants,
cube, multisampled) behind one descriptor and one ``GETexture`` handle. A
sampler is a separate object that controls *how* a shader reads the texture
(filtering, addressing, anisotropy). Sometimes a binding also needs a
*swizzle* — a remap of which texel channels appear as R / G / B / A inside
the shader.

This page walks through the four pieces in the order you need them:
picking the right kind, choosing pixel format and usage, sampling
behaviour, and channel swizzling.

.. contents:: On this page
   :local:
   :depth: 2

The four ingredients
====================

A bind site for a textured draw or dispatch needs:

1. A **texture** — the image data plus its *kind* (shape — 2D, cube, MS, …),
   *pixel format* (channel count + precision + colour space), and *usage*
   (who reads / writes it). Built from
   :cpp:struct:`OmegaGTE::TextureDescriptor`.
2. A **sampler** — how shaders read the texture. Built from
   :cpp:struct:`OmegaGTE::SamplerDescriptor`. One sampler can be shared
   across many textures.
3. An optional **swizzle** — a four-channel remap applied between the
   texture's stored layout and what the shader sees as ``.rgba``.
   :cpp:struct:`OmegaGTE::TextureSwizzle`.
4. A **region** (for partial uploads or blit copies) —
   :cpp:struct:`OmegaGTE::TextureRegion`.

Each lives in its own section below.

Picking a texture kind
======================

The descriptor's ``kind`` field tells the engine what shape the texture is.
This drives the native view dimension the backend creates (D3D12 SRV,
Vulkan ``VkImageView``, Metal ``MTLTexture``), and it is what bind
validation checks against the shader's compiled expectation. The shader
that samples a ``texture2d<float>`` expects to be bound a ``Tex2D``; bind a
``TexCube`` to that slot and the bind path writes a diagnostic to ``stderr``
and skips the bind.

.. cpp:enum-class:: OmegaGTE::TextureKind

    +-------------------+----------------------------------------------------+
    | ``Tex1D``         | One-dimensional texture (e.g. lookup tables).      |
    +-------------------+----------------------------------------------------+
    | ``Tex2D``         | Standard 2D image. Most common.                    |
    +-------------------+----------------------------------------------------+
    | ``Tex3D``         | Volume texture (e.g. 3D LUT, voxel data).          |
    +-------------------+----------------------------------------------------+
    | ``Tex1DArray``    | Array of 1D textures, indexed by array layer.      |
    +-------------------+----------------------------------------------------+
    | ``Tex2DArray``    | Array of 2D textures (e.g. shadow-map atlas).      |
    +-------------------+----------------------------------------------------+
    | ``TexCube``       | Six 2D faces forming a cubemap; ``arrayLayers``    |
    |                   | is fixed at 6.                                     |
    +-------------------+----------------------------------------------------+
    | ``TexCubeArray``  | Array of cubemaps; ``arrayLayers`` must be a       |
    |                   | multiple of 6.                                     |
    +-------------------+----------------------------------------------------+
    | ``Tex2DMS``       | Multisampled 2D — ``sampleCount > 1``. Used as a   |
    |                   | render target before resolving.                    |
    +-------------------+----------------------------------------------------+
    | ``Tex2DMSArray``  | Array of multisampled 2D textures.                 |
    +-------------------+----------------------------------------------------+
    | ``Auto``          | Back-compat sentinel for legacy code. Backends     |
    |                   | treat it as ``Tex2D``; **new code should always    |
    |                   | set ``kind`` explicitly**.                         |
    +-------------------+----------------------------------------------------+

Pixel formats
=============

The pixel format controls channel layout and precision. OmegaGTE supports
the cross-backend-portable intersection — five formats that work
identically on D3D12, Metal, and Vulkan:

.. cpp:enum-class:: OmegaGTE::PixelFormat

    +-----------------------------+------------------------------------------------------+
    | ``RGBA8Unorm``              | 8-bit RGBA, linear. Default for most textures.       |
    +-----------------------------+------------------------------------------------------+
    | ``RGBA16Unorm``             | 16-bit RGBA, linear. HDR intermediates, height maps. |
    +-----------------------------+------------------------------------------------------+
    | ``RGBA8Unorm_SRGB``         | 8-bit RGBA, gamma-encoded sRGB. Use for textures     |
    |                             | sampled as colour and displayed without further      |
    |                             | tone-mapping.                                        |
    +-----------------------------+------------------------------------------------------+
    | ``BGRA8Unorm``              | 8-bit BGRA, linear. The default swap-chain format    |
    |                             | (it is the only format universally accepted by       |
    |                             | every backend's surface).                            |
    +-----------------------------+------------------------------------------------------+
    | ``BGRA8Unorm_SRGB``         | 8-bit BGRA, gamma-encoded sRGB.                      |
    +-----------------------------+------------------------------------------------------+

The alias ``TexturePixelFormat`` is provided for descriptor-style code that
prefers a domain-named type; it is exactly ``PixelFormat``.

Allocating a texture
====================

.. cpp:class:: OmegaGTE::GETexture

    A GPU texture resource.

    .. cpp:function:: TextureKind getKind() const
    .. cpp:function:: unsigned getArrayLayers() const
    .. cpp:function:: unsigned getSampleCount() const

        Effective shape of the live texture. Bind paths read these to
        validate the bind against the shader's declared expectation.

    .. cpp:function:: void copyBytes(void *bytes, size_t bytesPerRow)

        Upload to **mip 0** of the texture, covering the full extent.
        Only valid for ``ToGPU`` textures. ``bytesPerRow`` is
        ``width * bytesPerPixel``.

    .. cpp:function:: void copyBytes(void *bytes, size_t bytesPerRow, const TextureRegion & destRegion)

        Upload to a sub-region of mip 0. ``bytes`` must point at a tightly
        packed block sized for the region (``bytesPerRow * h * max(d, 1)``).
        For a 2D texture set ``destRegion.z = 0`` and ``destRegion.d = 1``.
        Lets you stream rectangular tiles into a larger texture without
        reuploading the whole image.

    .. cpp:function:: size_t getBytes(void *bytes, size_t bytesPerRow)

        Download to ``bytes`` from a ``FromGPU`` texture. Pass
        ``bytes = nullptr`` to query the required CPU buffer size without
        reading.

.. cpp:struct:: OmegaGTE::TextureDescriptor

    Inputs to ``OmegaGraphicsEngine::makeTexture`` (or a heap's
    ``makeTexture``).

    .. cpp:member:: StorageOpts storage_opts

        ``Shared`` (CPU + GPU accessible memory) or ``GPUOnly``. Default
        ``Shared``. Most usages override this implicitly via ``usage``;
        override only for unusual cases.

    .. cpp:member:: GETexture::GETextureUsage usage

        See *Picking a usage* below. Default ``ToGPU``.

    .. cpp:member:: TexturePixelFormat pixelFormat

        Default ``RGBA8Unorm``.

    .. cpp:member:: unsigned width
    .. cpp:member:: unsigned height
    .. cpp:member:: unsigned depth

        Texture extent. ``depth`` defaults to ``1``; set > 1 only for
        ``Tex3D``.

    .. cpp:member:: unsigned mipLevels

        Number of mipmap levels. ``1`` means no mips. The engine does not
        auto-generate mips for you — fill each level via a blit pass or
        per-level ``copyBytes`` calls.

    .. cpp:member:: unsigned sampleCount

        ``1`` for normal textures; ``4`` or ``8`` for MSAA (must be ≤
        :cpp:member:`GTEDeviceFeatures::maxMSAASamples`). Used in concert
        with ``kind = Tex2DMS`` (or ``Tex2DMSArray``).

    .. cpp:member:: TextureKind kind

        The shape — see *Picking a texture kind* above. Default ``Tex2D``.

    .. cpp:member:: unsigned arrayLayers

        Layer count for array kinds. Fixed at ``6`` for ``TexCube``; must
        be a multiple of 6 for ``TexCubeArray``; the layer count for
        ``Tex1DArray`` / ``Tex2DArray`` / ``Tex2DMSArray``. Ignored for
        non-array kinds.

    .. cpp:member:: TextureSwizzle defaultSwizzle

        Channel remap baked into the texture's primary view at creation
        time. Every bind without a runtime swizzle override sees the
        swizzled channels for free. See *Channel swizzling* below.
        Defaults to identity (no remapping).

Picking a usage
===============

``GETextureUsage`` declares the access pattern — used by the backend to
pick the right memory residency and to set up resource transitions.

+-----------------------------------+--------------------------------------------------------+
| ``ToGPU``                         | CPU writes, GPU reads. Static asset textures, fonts,   |
|                                   | UI atlases.                                            |
+-----------------------------------+--------------------------------------------------------+
| ``FromGPU``                       | GPU writes, CPU reads. Use to download computed        |
|                                   | pixels back to the CPU (screenshots, debugging).       |
+-----------------------------------+--------------------------------------------------------+
| ``GPUAccessOnly``                 | GPU writes and reads; the CPU never touches it.        |
|                                   | Faster than ``ToGPU`` after the initial upload (lives  |
|                                   | in device-local memory). Use for intermediate          |
|                                   | render targets, post-process buffers.                  |
+-----------------------------------+--------------------------------------------------------+
| ``RenderTarget``                  | Bound as a colour attachment in a render pass.         |
|                                   | Implies GPU-private storage.                           |
+-----------------------------------+--------------------------------------------------------+
| ``MSResolveSrc``                  | Source texture for an MSAA resolve operation in a      |
|                                   | render pass (see :doc:`RenderPipeline`).               |
+-----------------------------------+--------------------------------------------------------+
| ``RenderTargetAndDepthStencil``   | Combined colour + depth/stencil attachment.            |
+-----------------------------------+--------------------------------------------------------+

.. code-block:: cpp

    // Diffuse texture asset uploaded from CPU
    OmegaGTE::TextureDescriptor desc;
    desc.kind        = OmegaGTE::TextureKind::Tex2D;
    desc.usage       = OmegaGTE::GETexture::ToGPU;
    desc.pixelFormat = OmegaGTE::PixelFormat::RGBA8Unorm_SRGB;
    desc.width       = 512;
    desc.height      = 512;
    desc.mipLevels   = 1;
    auto diffuse = gte.graphicsEngine->makeTexture(desc);
    diffuse->copyBytes(pixelData, 512 * 4);

    // Off-screen render target
    OmegaGTE::TextureDescriptor rtDesc;
    rtDesc.kind        = OmegaGTE::TextureKind::Tex2D;
    rtDesc.usage       = OmegaGTE::GETexture::RenderTarget;
    rtDesc.pixelFormat = OmegaGTE::PixelFormat::RGBA8Unorm;
    rtDesc.width       = 1920;
    rtDesc.height      = 1080;
    auto offscreen = gte.graphicsEngine->makeTexture(rtDesc);

    // Cubemap for an environment probe
    OmegaGTE::TextureDescriptor cubeDesc;
    cubeDesc.kind        = OmegaGTE::TextureKind::TexCube;
    cubeDesc.usage       = OmegaGTE::GETexture::ToGPU;
    cubeDesc.pixelFormat = OmegaGTE::PixelFormat::RGBA16Unorm;
    cubeDesc.width       = 256;
    cubeDesc.height      = 256;
    cubeDesc.arrayLayers = 6;     // required for TexCube
    auto envProbe = gte.graphicsEngine->makeTexture(cubeDesc);

    // 4x MSAA colour target, paired with a 1x resolve texture later
    OmegaGTE::TextureDescriptor msDesc;
    msDesc.kind        = OmegaGTE::TextureKind::Tex2DMS;
    msDesc.usage       = OmegaGTE::GETexture::RenderTarget;
    msDesc.pixelFormat = OmegaGTE::PixelFormat::RGBA8Unorm;
    msDesc.width       = 1920;
    msDesc.height      = 1080;
    msDesc.sampleCount = 4;
    auto msTarget = gte.graphicsEngine->makeTexture(msDesc);

Texture regions
===============

A region is a sub-rectangle (or sub-box) of a texture used by partial
uploads and by blit copies (see :doc:`Blitting`).

.. cpp:struct:: OmegaGTE::TextureRegion

    .. cpp:member:: unsigned x
    .. cpp:member:: unsigned y
    .. cpp:member:: unsigned z

        Origin of the region.

    .. cpp:member:: unsigned w
    .. cpp:member:: unsigned h
    .. cpp:member:: unsigned d

        Dimensions of the region.

    .. cpp:member:: unsigned mipLevel

        Mip pyramid level this region addresses. Defaults to ``0`` so
        six-field aggregate initialisers keep targeting the base level.

    .. cpp:member:: unsigned arrayLayer

        Array slice (or cube-face index) this region addresses. Defaults
        to ``0``.

Samplers
========

A sampler controls how a shader fetches texels from a texture: which
interpolation, how the texture wraps at its edges, and how anisotropic
filtering behaves at glancing angles. Samplers are independent of textures
— one sampler can serve many textures.

.. cpp:class:: OmegaGTE::GESamplerState

    Opaque sampler object. Bind to a render or compute pass via
    ``bindResourceAtVertexShader`` / ``bindResourceAtFragmentShader`` /
    ``bindResourceAtComputeShader`` (see :doc:`RenderPipeline` and
    :doc:`ComputePipeline`).

.. cpp:struct:: OmegaGTE::SamplerDescriptor

    .. cpp:member:: OmegaCommon::StrRef name

        Optional debug label shown in GPU profilers.

    .. cpp:member:: AddressMode uAddressMode
    .. cpp:member:: AddressMode vAddressMode
    .. cpp:member:: AddressMode wAddressMode

        Addressing behaviour for each texture axis at UV values outside
        ``[0, 1]``. Default ``Wrap``.

        +------------------------------+--------------------------------------------+
        | ``Wrap``                     | Tile the texture in both directions.       |
        +------------------------------+--------------------------------------------+
        | ``ClampToEdge``              | Stretch the edge pixel outwards.           |
        +------------------------------+--------------------------------------------+
        | ``MirrorClampToEdge``        | Mirror once, then clamp.                   |
        +------------------------------+--------------------------------------------+
        | ``MirrorWrap``               | Alternate flipped / unflipped tiles.       |
        +------------------------------+--------------------------------------------+

    .. cpp:member:: Filter filter

        Filtering used for magnification, minification, and between mip
        levels.

        +--------------------------------------+----------------------------------------------+
        | ``Linear``                           | Bilinear (no mips) / trilinear (with mips).  |
        +--------------------------------------+----------------------------------------------+
        | ``Point``                            | Nearest-neighbour. Pixel-art textures, UI    |
        |                                      | atlases where bleeding is undesirable.       |
        +--------------------------------------+----------------------------------------------+
        | ``MaxAnisotropic``                   | Anisotropic filtering at the device's max    |
        |                                      | quality. Best for terrain and large planar   |
        |                                      | surfaces seen at glancing angles.            |
        +--------------------------------------+----------------------------------------------+
        | ``MinAnisotropic``                   | Anisotropic at the minimum quality.          |
        +--------------------------------------+----------------------------------------------+
        | ``MagLinearMinPointMipLinear`` …     | Mixed mag / min / mip filter modes (six      |
        |                                      | combinations) for fine control.              |
        +--------------------------------------+----------------------------------------------+

    .. cpp:member:: unsigned int maxAnisotropy

        Anisotropy cap (default 16). Bounded by
        :cpp:member:`GTEDeviceFeatures::maxSamplerAnisotropy`. Has no
        effect unless ``filter`` is one of the anisotropic modes.

.. code-block:: cpp

    OmegaGTE::SamplerDescriptor linearWrap;
    linearWrap.name         = "linearWrap";
    linearWrap.filter       = OmegaGTE::SamplerDescriptor::Filter::Linear;
    linearWrap.uAddressMode = OmegaGTE::SamplerDescriptor::AddressMode::Wrap;
    linearWrap.vAddressMode = OmegaGTE::SamplerDescriptor::AddressMode::Wrap;
    auto sampLinear = gte.graphicsEngine->makeSamplerState(linearWrap);

    // Anisotropic sampler for terrain at glancing angles
    OmegaGTE::SamplerDescriptor aniso;
    aniso.name           = "terrainAniso16";
    aniso.filter         = OmegaGTE::SamplerDescriptor::Filter::MaxAnisotropic;
    aniso.maxAnisotropy  = 16;
    auto sampAniso = gte.graphicsEngine->makeSamplerState(aniso);

Channel swizzling
=================

A swizzle is a four-channel remap applied between the texture's stored
layout and the four-component value the shader sees as ``.rgba``. The most
common uses: turning a single-channel ``R`` texture into a greyscale visual
by broadcasting the red channel across RGB, or fixing up a content pipeline
that delivers BGRA bytes for a shader expecting RGBA.

.. cpp:enum-class:: OmegaGTE::TextureSwizzleChannel

    Source for one output channel.

    +--------------+---------------------------------------------------+
    | ``Red``      | Source the texture's red channel.                 |
    +--------------+---------------------------------------------------+
    | ``Green``    | Source the texture's green channel.               |
    +--------------+---------------------------------------------------+
    | ``Blue``     | Source the texture's blue channel.                |
    +--------------+---------------------------------------------------+
    | ``Alpha``    | Source the texture's alpha channel.               |
    +--------------+---------------------------------------------------+
    | ``Zero``     | Constant 0.                                       |
    +--------------+---------------------------------------------------+
    | ``One``      | Constant 1.                                       |
    +--------------+---------------------------------------------------+
    | ``Identity`` | Passthrough — use the channel's own position      |
    |              | (e.g. red goes through unchanged in the R slot).  |
    +--------------+---------------------------------------------------+

.. cpp:struct:: OmegaGTE::TextureSwizzle

    .. cpp:member:: TextureSwizzleChannel r
    .. cpp:member:: TextureSwizzleChannel g
    .. cpp:member:: TextureSwizzleChannel b
    .. cpp:member:: TextureSwizzleChannel a

        Source channel for each output. Default ``Identity`` (no remap).

    .. cpp:function:: static TextureSwizzle identity()

        Convenience: identity swizzle.

    .. cpp:function:: static TextureSwizzle broadcastRed()

        Convenience: red into all four output channels. Use to render a
        single-channel texture as greyscale.

    .. cpp:function:: static TextureSwizzle swapRB()

        Convenience: swap R and B (RGBA → BGRA channel order). Useful for
        importing assets authored in the wrong channel order without
        re-baking them.

    .. cpp:function:: bool isIdentity() const

        ``true`` when all four sources are ``Identity``.

A swizzle baked into a texture descriptor via ``defaultSwizzle`` is applied
to every bind that does not specify a per-bind override; backends allocate
the primary view (D3D12 SRV, Metal ``MTLTexture``, Vulkan ``VkImageView``)
with this mapping so the cost is paid at allocation time, not per bind.

.. code-block:: cpp

    // R8-style font atlas stored as RGBA but only the red channel carries data.
    OmegaGTE::TextureDescriptor fontDesc;
    fontDesc.kind            = OmegaGTE::TextureKind::Tex2D;
    fontDesc.usage           = OmegaGTE::GETexture::ToGPU;
    fontDesc.pixelFormat     = OmegaGTE::PixelFormat::RGBA8Unorm;
    fontDesc.width           = 1024;
    fontDesc.height          = 1024;
    fontDesc.defaultSwizzle  = OmegaGTE::TextureSwizzle::broadcastRed();
    auto fontAtlas = gte.graphicsEngine->makeTexture(fontDesc);
    // Shaders sampling fontAtlas see (R,R,R,R) without any extra work.

Common pitfalls
===============

* **Leaving ``kind`` at ``Auto``.** It works for ``Tex2D`` but breaks the
  moment you actually need a cube or an MS texture — the backend falls
  back to ``Tex2D`` silently. Always set ``kind`` explicitly.
* **Forgetting ``arrayLayers``.** ``TexCube`` requires ``6`` and
  ``TexCubeArray`` requires a multiple of ``6``. Wrong values fail at
  resource creation.
* **Sample-count mismatch between texture and pipeline.** A multisampled
  bind site (``Tex2DMS``) requires ``sampleCount > 1`` on the bound
  texture; the bind path emits a diagnostic and skips the bind otherwise.
* **Calling** ``copyBytes`` **on a non-ToGPU texture.** The function is
  ``ToGPU``-only. For other usages, populate via a compute pass or a blit
  copy.
* **Using ``RGBA8Unorm`` as the swap-chain format.** Native render targets
  require the cross-backend portable intersection — ``BGRA8Unorm`` is the
  only format universally supported. ``RGBA8Unorm`` is fine for off-screen
  textures.
