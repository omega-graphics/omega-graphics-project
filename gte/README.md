# OmegaGTE (Omega Graphics and Tessellation Engine)
A cross platform low level api for rendering 3D graphics, performing computations on a gpu, and performing tessalations on 2D and 3D geometric primatives. This api is paticularly useful for designing cross platform user interface apis, video renderers, and even 3D game engines.

This repo also features a cross platform shading language OmegaSL.

### Building and running 2DTest (macOS Metal)

From the **repository root** (not the `gte` folder):

```bash
mkdir -p build && cd build
cmake .. -G Ninja
ninja OmegaGTE omegaslc 2DTest
```

Then run the app (requires a display/GPU; will show a window with a red rectangle):

```bash
open gte/tests/metal/2DTest.app
# or run the binary directly:
# gte/tests/metal/2DTest.app/Contents/MacOS/2DTest
```

To build only GTE and 2DTest without the rest of the suite (e.g. if WTK dependencies are missing), use the `ninja` targets above; the full `ninja` may fail on missing optional deps.

See [OmegaSL](./omegasl)


Apache License 2.0


#### Why no OpenGL support?

The latest update of OpenGL was released in 2017 and as Vulkan gets more mature, the Khronos Group is slowly fading out support for OpenGL and OpenGL ES. Both APIs are well implemented however OpenGL, being almost 30 years old, has a deprecated api from when graphics hardware was more primitive. Additionally, Vulkan has proper builtin support for Ray Tracing.