==============
Render Targets
==============

A render target is the destination surface for a render pass. OmegaGTE has
two flavours and you pick by *where the result needs to land*:

* :cpp:class:`OmegaGTE::GENativeRenderTarget` — draws into a platform
  window's swap chain. The result becomes a frame on the screen via
  ``present()``. Use one per output window.
* :cpp:class:`OmegaGTE::GETextureRenderTarget` — draws into an off-screen
  :cpp:class:`GETexture`. Use for shadow maps, post-process passes,
  environment probes, anything you sample again later.

Both subclass the same opaque :cpp:class:`OmegaGTE::GERenderTarget` base,
so the render-pass encoding API on either looks the same (see
:doc:`RenderPipeline`). The difference is purely how the result is *fed
out* once drawing is done.

.. contents:: On this page
   :local:
   :depth: 2

GENativeRenderTarget
====================

A native render target wraps a platform-native surface (an ``HWND`` on
Windows, a ``CAMetalLayer`` on Apple, a ``VkSurfaceKHR`` derived from an
X11 / Wayland / Android window on Linux/Android) and exposes it as one
back buffer at a time. Drawing into it is exactly like drawing into a
texture target — the difference is the ``present()`` step at the end,
which flips the back buffer to the screen.

The native target is **bound to a present queue** at creation time. The
queue you pass to ``makeNativeRenderTarget`` is the one ``present()``
submits the final transition + Present on; on D3D12 and Vulkan the swap
chain is created against that queue and cannot be re-targeted later. The
caller is responsible for having submitted all the draw work for the
frame to the same queue before calling ``present()``.

.. cpp:class:: OmegaGTE::GENativeRenderTarget

    .. cpp:function:: PixelFormat pixelFormat()

        The pixel format of the swap-chain back buffer. Defaults to
        ``BGRA8Unorm``; settable via
        :cpp:member:`NativeRenderTargetDescriptor::pixelFormat` with the
        constraint that the format must be in the cross-backend portable
        set.

    .. cpp:function:: SharedHandle<GECommandQueue> presentQueue() const

        The command queue this target was created against. Use it to look
        up which queue to submit frame work on.

    .. cpp:function:: void present()

        Submit the engine's internal "transition the back buffer to
        Present state and flip" work on the present queue. Replaces the
        older ``commitAndPresent()`` entry point. Call once per frame
        **after** you have submitted the draw work that fills the back
        buffer to the same queue.

    *Windows-only:*

    .. cpp:function:: void *getSwapChain()

        Returns the underlying ``IDXGISwapChain1 *`` (D3D11 path) or
        ``IDXGISwapChain3 *`` (D3D12 path). Use to drop down to direct
        swap-chain manipulation when OmegaGTE does not wrap something.

    .. cpp:function:: void resizeSwapChain(unsigned int width, unsigned int height)

        Wait for the GPU to finish, resize the swap chain, and recreate
        render target views. Call this instead of
        ``IDXGISwapChain::ResizeBuffers`` in response to ``WM_SIZE`` — the
        wrapper handles the resource-state dance for you.

    .. cpp:function:: void waitForGPU()

        Block the CPU until the present queue's last submission has
        completed. Typically you call this before tearing the window
        down. (Long term these will move to ``GECommandQueue``; today they
        live on the Windows target as a transitional convenience.)

    .. cpp:function:: void waitForFence(SharedHandle<GEFence> & fence)

        CPU-side wait for a fence signal. Useful when a texture produced
        on a different queue feeds into a draw on this target.

.. cpp:struct:: OmegaGTE::NativeRenderTargetDescriptor

    Inputs to :cpp:func:`OmegaGraphicsEngine::makeNativeRenderTarget`. Most
    fields are platform-conditional — fill in only the ones your build
    targets.

    .. cpp:member:: bool allowDepthStencilTesting

        Must be ``true`` if you intend to use depth or stencil testing in
        pipelines bound to this target. Allocates the depth buffer at
        creation time. Default ``false``.

    .. cpp:member:: PixelFormat pixelFormat

        Swap-chain colour format. Default ``BGRA8Unorm`` — the only format
        universally supported across D3D12, Metal, and Vulkan surfaces.
        Setting this to something outside the portable set
        (:cpp:func:`isPortableNativeRenderTargetFormat`) will fail on at
        least one backend. The current portable set is ``BGRA8Unorm`` and
        ``BGRA8Unorm_SRGB``.

    *DirectX (Windows):*

    .. cpp:member:: bool isHwnd
    .. cpp:member:: HWND hwnd
    .. cpp:member:: unsigned width
    .. cpp:member:: unsigned height

    *Metal (macOS / iOS, Objective-C TUs only):*

    .. cpp:member:: CAMetalLayer *metalLayer

    *Vulkan (Linux / Android):*

    X11: ``Window x_window``, ``Display *x_display``.

    Wayland: ``wl_surface *``, ``wl_display *``, ``unsigned width``,
    ``unsigned height``.

    Android: ``ANativeWindow *window``.

.. cpp:function:: bool OmegaGTE::isPortableNativeRenderTargetFormat(PixelFormat fmt)

    Returns ``true`` for ``PixelFormat`` values that are valid swap-chain
    formats on every backend OmegaGTE supports. Call before assigning
    :cpp:member:`NativeRenderTargetDescriptor::pixelFormat` if your code
    can be retargeted to any backend.

.. code-block:: cpp

    // Windows
    auto queue = gte.graphicsEngine->makeCommandQueue(3);

    OmegaGTE::NativeRenderTargetDescriptor desc;
    desc.isHwnd                   = true;
    desc.hwnd                     = hWnd;
    desc.width                    = clientWidth;
    desc.height                   = clientHeight;
    desc.allowDepthStencilTesting = true;
    desc.pixelFormat              = OmegaGTE::PixelFormat::BGRA8Unorm;

    auto nativeRT = gte.graphicsEngine->makeNativeRenderTarget(desc, queue);

    // …submit frame draw work onto `queue` (the target's present queue)…

    nativeRT->present();      // flips the back buffer to the screen

    // On WM_SIZE:
    nativeRT->resizeSwapChain(newWidth, newHeight);

GETextureRenderTarget
=====================

A texture render target draws into a :cpp:class:`GETexture`. The texture is
either supplied by the caller (most common — you bind the same shadow-map
texture as the target one frame and as a shader input the next) or
allocated for you by the engine.

The texture target is much smaller than the native one. It does not own a
command queue — whichever queue you submit the encoded command buffer onto
is the queue responsible for the texture. Synchronisation between writing
to the texture and sampling it later is done by :cpp:class:`GEFence` and
:cpp:class:`GECommandQueue`'s wait/signal API; see :doc:`GPUSubmission`.

.. cpp:class:: OmegaGTE::GETextureRenderTarget

    .. cpp:function:: SharedHandle<GETexture> underlyingTexture()

        The texture this target writes into. Use as a shader input in a
        subsequent pass.

.. cpp:struct:: OmegaGTE::TextureRenderTargetDescriptor

    .. cpp:member:: bool renderToExistingTexture

        When ``true``, the engine uses the supplied :cpp:member:`texture`
        as the target. When ``false``, the engine allocates a fresh
        texture from :cpp:member:`region` and the pixel format inherited
        from the bound pipeline.

    .. cpp:member:: SharedHandle<GETexture> texture

        The texture to render into when :cpp:member:`renderToExistingTexture`
        is ``true``. Must be created with one of the render-target
        usages (``RenderTarget`` or ``RenderTargetAndDepthStencil``).

    .. cpp:member:: TextureRegion region

        Sub-region of the texture to render into. For full-texture
        targets, the region covers the whole image.

.. code-block:: cpp

    // 1024x1024 shadow map
    OmegaGTE::TextureDescriptor texDesc;
    texDesc.kind        = OmegaGTE::TextureKind::Tex2D;
    texDesc.usage       = OmegaGTE::GETexture::RenderTarget;
    texDesc.pixelFormat = OmegaGTE::PixelFormat::RGBA8Unorm;
    texDesc.width       = 1024;
    texDesc.height      = 1024;
    auto shadowMapTex = gte.graphicsEngine->makeTexture(texDesc);

    OmegaGTE::TextureRenderTargetDescriptor trtDesc;
    trtDesc.renderToExistingTexture = true;
    trtDesc.texture                 = shadowMapTex;
    trtDesc.region = { 0, 0, 0, 1024, 1024, 1 };

    auto shadowRT = gte.graphicsEngine->makeTextureRenderTarget(trtDesc);

    // Pass 1: render the shadow map onto shadowRT (queue 'shadowQueue').
    //   …encode + submit on shadowQueue, signal a fence on completion…

    // Pass 2: bind shadowMapTex as a shader input on the main pass after
    //         waiting on the fence (see :doc:`GPUSubmission`).

Common pitfalls
===============

* **Submitting frame work on a different queue than the present queue.**
  ``present()`` flips the back buffer that the present queue has written;
  if you drew on a different queue, the swap chain shows last frame's
  contents. Submit frame work on ``nativeRT->presentQueue()``.
* **Calling ``commitAndPresent()``.** That entry point was removed. Submit
  the frame's draw work to the present queue yourself, then call
  ``present()``.
* **Picking a non-portable pixel format for a native target.** Use
  ``isPortableNativeRenderTargetFormat()`` to check. The current portable
  set is ``BGRA8Unorm`` and ``BGRA8Unorm_SRGB`` — Metal's ``CAMetalLayer``
  rejects RGBA-channel-order formats.
* **Resizing the swap chain by calling Direct3D's resize directly.** Use
  :cpp:func:`GENativeRenderTarget::resizeSwapChain` so OmegaGTE can wait
  for outstanding GPU work and rebuild RTVs.
* **Sampling a texture render target before the GPU has finished writing
  it.** Use a fence between the producer submit and the consumer submit
  (see :doc:`GPUSubmission`). The texture is only safe to read once the
  fence has been signalled.
