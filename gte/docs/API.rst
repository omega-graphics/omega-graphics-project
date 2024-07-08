===
API
===

This page is collection of all public classes in OmegaGTE.

.. cpp:namespace:: OmegaGTE

.. cpp:class:: GTEDevice

A device that can perform matrix operations.
**Graphics API**


.. cpp:class:: OmegaGraphicsEngine

The Main graphics engine class.

.. cpp:class:: GEBuffer

A buffer allocated on the GPU.

.. cpp:class:: GETexture

A buffer that holds sequential 1D, 2D, or 3D texel data.

.. cpp:class:: GECommandQueue

A statically allocated queue of command buffers. Responsible for uploading command buffers to the GTEDevice.

.. cpp:class:: GECommandBuffer

A list of buffered GPU commands that are executed sequentially.

.. cpp:class:: GENativeRenderTarget

A high-level wrapper class around the GECommandQueue for rendering to a native render target.
(HWND on Windows, CAMetalLayer on Apple Devices,
ANativeWindow on Android, X11 Window on Linux with X11 Support,
and wl_surface on Linux with Wayland support.)

.. cpp:class:: GETextureRenderTarget

**Tesselation API**