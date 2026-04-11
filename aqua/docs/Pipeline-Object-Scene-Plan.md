# AQUA Pipeline, Object, Scene — Implementation Plan (GTE Isolation)

## Context

BasicGame currently works with raw GTE command buffers, render pass descriptors, and pipeline state objects. The goal is to add abstractions so users never touch GTE directly — **and so GTE headers never appear in AQUA's public API**. This keeps translation units small as AQUA grows.

**Architecture:**

```
User code (BasicGame)
  |  only includes aqua/ headers
Public API: App.h, Pipeline.h, Object.h, Scene.h, Math.h
  |  implementation boundary
Internal: src/pipeline/, src/renderer/
  |  only these .cpp files include OmegaGTE
OmegaGTE (GE.h, GEPipeline.h, GERenderTarget.h, GTEShader.h, etc.)
```

GEMesh does not exist yet (see `gte/docs/GEMesh-TextureAssets-Implementation-Plan.md`). Object forward-declares an opaque mesh handle; full rendering activates when GEMesh lands.

---

## New Files

| File | Purpose |
|------|---------|
| `aqua/include/aqua/Math.h` | AQUA math types (Vec3, Vec4, Mat4, Color) — no GTE includes |
| `aqua/include/aqua/Pipeline.h` | Pipeline public API (opaque handle, PipelineDesc with AQUA-only types) |
| `aqua/include/aqua/Object.h` | Object public API (transform, visibility, name) |
| `aqua/include/aqua/Scene.h` | Scene public API (hierarchy, camera, clear color, render) |
| `aqua/src/pipeline/Pipeline.cpp` | Pipeline impl — includes GTE, compiles shaders, creates pipeline state |
| `aqua/src/renderer/Renderer.h` | Internal renderer class (not public) — owns command encoding |
| `aqua/src/renderer/Renderer.cpp` | Renderer impl — includes GTE, manages render passes |
| `aqua/src/Object.cpp` | Object impl (getters/setters, pure data) |
| `aqua/src/Scene.cpp` | Scene impl (hierarchy, transform propagation, delegates to Renderer) |
| `aqua/src/Math.cpp` | Mat4 utilities (perspective, lookAt, translation, etc.) |

## Modified Files

| File | Change |
|------|--------|
| `aqua/include/aqua/App.h` | Remove `#include <OmegaGTE.h>`, remove `gte()` and `renderTarget()` accessors, add `createPipeline()` / `createPipelineFromLibrary()` factories |
| `aqua/src/App.cpp` | Add pipeline factory methods that forward to Pipeline::create with internal GTE ref |
| `aqua/CMakeLists.txt` | Add new source files to SOURCES |
| `aqua/tests/BasicGame.cpp` | Rewrite to use only AQUA types (no GTE includes) |

---

## 1. Math Types

**Header:** `aqua/include/aqua/Math.h`

AQUA-owned types so public headers never reference `FMatrix`/`FVec` from GTE.

```
Vec3 { x, y, z }
Vec4 { x, y, z, w }
Color { r, g, b, a }

Mat4
  data[16]  (column-major)
  identity()
  perspective(fovRadians, aspect, near, far)
  lookAt(eye, target, up)
  translation(t)
  rotation(angleRadians, axis)
  scale(s)
  operator*
```

**Impl (`src/Math.cpp`):** Calls GTE math functions (`perspectiveProjection`, `lookAt`, `translationMatrix`, etc.) internally and copies results into `Mat4::data`. This is the only math file that includes GTE headers.

---

## 2. Pipeline

**Header:** `aqua/include/aqua/Pipeline.h`

```
CullMode  { None, Front, Back }
FillMode  { Solid, Wireframe }

PipelineDesc
  cullMode          (CullMode, default Back)
  fillMode          (FillMode, default Solid)
  enableDepth       (bool, default false)
  vertexFunction    (string — name in shader library)
  fragmentFunction  (string — name in shader library)

Pipeline
  ~Pipeline()
  // No public constructors — created via App::createPipeline()
```

**Impl (`src/pipeline/Pipeline.cpp`):** Includes GTE headers. Holds `SharedHandle<GTEShaderLibrary>` and `SharedHandle<GERenderPipelineState>`.

Internal static factory (called by App):
- `create(GTE &gte, omegaslPath, desc)` — runtime compile
- `createFromLibrary(GTE &gte, libPath, desc)` — pre-compiled

**`create()` flow:**
1. Read `.omegasl` source from file
2. `gte.omegaSlCompiler->compile(...)` (guarded by `#if RUNTIME_SHADER_COMP_SUPPORT`)
3. `gte.graphicsEngine->loadShaderLibraryRuntime(lib)` -> `GTEShaderLibrary`
4. Look up vertex/fragment shaders by name from `shaderLib->shaders`
5. Map AQUA `CullMode`/`FillMode` -> GTE `RasterCullMode`/`TriangleFillMode`
6. Fill `RenderPipelineDescriptor` + call `makeRenderPipelineState()`

**`createFromLibrary()` flow:** Same but step 2-3 replaced by `loadShaderLibrary(path)`.

---

## 3. App.h Changes

Remove `#include <OmegaGTE.h>`, remove `gte()` and `renderTarget()` accessors.

```
App
  App(desc)
  ~App()
  window()
  createPipeline(omegaslPath, desc)         -> shared_ptr<Pipeline>
  createPipelineFromLibrary(libPath, desc)   -> shared_ptr<Pipeline>
  onInit()
  onFrame()
  run()
```

`App::Impl` (in `src/App.cpp`) still owns `OmegaGTE::GTE` and the native render target internally. The `createPipeline` methods forward to `Pipeline::create(impl->gte, ...)`.

Scene accesses the internal Renderer through `App` via friendship.

---

## 4. Renderer (Internal)

**Header:** `aqua/src/renderer/Renderer.h` (NOT in `include/` — internal only)

```
Renderer
  Renderer(renderTarget)
  beginFrame(clearColor)
  // Future: draw(pipeline, mesh, mvp)
  endFrameAndPresent()
```

**Impl (`src/renderer/Renderer.cpp`):** Includes all GTE render target / command buffer headers.

- `beginFrame()`: Gets command buffer from render target, starts render pass with clear color
- `endFrameAndPresent()`: Ends render pass, submits, presents
- Future `draw()`: Sets pipeline state, binds vertex buffer, draws — activates when GEMesh lands

The Renderer is created by `App::Impl` during init and stored internally.

---

## 5. Object

**Header:** `aqua/include/aqua/Object.h`

```
Object (enable_shared_from_this)
  create(pipeline)            -> shared_ptr<Object>
  setTransform / transform()  Mat4 (local space, default Identity)
  setPipeline / pipeline()    shared_ptr<Pipeline>
  setVisible / isVisible()    bool (default true)
  setName / name()            string (debug)
  ~Object()
```

Pure data — no GPU commands, no GTE types in the header. Mesh added when GEMesh lands.

---

## 6. Scene

**Header:** `aqua/include/aqua/Scene.h`

```
Scene
  create()                                 -> shared_ptr<Scene>
  add(object, parent=nullptr)              hierarchy insert
  remove(object)                           hierarchy remove
  setViewMatrix(mat)                       camera view
  setProjectionMatrix(mat)                 camera projection
  setClearColor(color)                     background
  render(app)                              encode + submit + present
  ~Scene()
```

**Internal scene graph:** Flat `std::vector<Node>` where each Node is `{shared_ptr<Object>, shared_ptr<Object> parent, Mat4 cachedWorld}`.

**`render(App &app)` flow:**
1. Compute world transforms: walk nodes, `world = parent ? parentWorld * local : local`
2. Get Renderer from `app.impl->renderer`
3. `renderer.beginFrame(clearColor)`
4. *(Future — when GEMesh lands)* For each visible object, sorted by pipeline:
   - Compute MVP = projection * view * world
   - `renderer.draw(pipeline, mesh, mvp)`
5. `renderer.endFrameAndPresent()`

Until GEMesh exists, step 4 is skipped — the scene just clears and presents.

---

## 7. CMakeLists.txt Change

```cmake
add_omega_graphics_module(AQUA SHARED
    HEADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include
    SOURCES
        src/App.cpp
        src/Math.cpp
        src/pipeline/Pipeline.cpp
        src/renderer/Renderer.cpp
        src/Object.cpp
        src/Scene.cpp
        ${PLATFORM_SRCS}
    DEPENDS OmegaGTE
)
```

---

## 8. BasicGame.cpp After

Uses only AQUA types — no OmegaGTE includes.

```cpp
class BasicGame : public Aqua::App {
    std::shared_ptr<Aqua::Scene> scene;
public:
    BasicGame() : App({{.title = "AQUA - BasicGame", .width = 1280, .height = 720}}) {}

    void onInit() override {
        scene = Aqua::Scene::create();
        scene->setClearColor({0.1f, 0.1f, 0.1f, 1.0f});
        scene->setProjectionMatrix(
            Aqua::Mat4::perspective(60.f * 3.14159f / 180.f, 1280.f/720.f, 0.1f, 100.f));
        scene->setViewMatrix(
            Aqua::Mat4::lookAt({0,2,5}, {0,0,0}, {0,1,0}));
    }

    void onFrame() override {
        scene->render(*this);
    }
};
```

Objects with meshes added once GEMesh is implemented.

---

## 9. Implementation Order

1. **Math.h / Math.cpp** — standalone, no dependencies on other new files
2. **Renderer** — internal, needs GTE (already in App.cpp's TU)
3. **Pipeline** — needs GTE internally, public header uses only AQUA types
4. **App.h changes** — remove GTE include, add pipeline factories, friend Scene
5. **Object** — pure data, uses Math.h
6. **Scene** — ties everything together, delegates rendering to Renderer
7. **BasicGame.cpp** — rewrite to use only AQUA types
8. **CMakeLists.txt** — add all new source files

---

## 10. GTE Isolation Summary

| AQUA public header | GTE types referenced | Status |
|---------------------|---------------------|--------|
| `App.h` | None (currently has `OmegaGTE.h`) | **Remove GTE include** |
| `Window.h` | Forward-decl `NativeRenderTargetDescriptor` only | Already isolated |
| `Pipeline.h` | None | New — uses AQUA enums |
| `Object.h` | None | New — uses `Aqua::Mat4` |
| `Scene.h` | None | New — uses `Aqua::Mat4`, `Aqua::Color` |
| `Math.h` | None | New — self-contained |

**Only these `.cpp` files include GTE:**
- `src/App.cpp` (owns GTE instance)
- `src/Math.cpp` (wraps GTE math)
- `src/pipeline/Pipeline.cpp` (shader compilation, pipeline state)
- `src/renderer/Renderer.cpp` (command buffer, render passes)
- `src/platform/macos/CocoaWindow.mm` (fills NativeRenderTargetDescriptor)

---

## 11. Verification

1. **Build:** `cmake --build` succeeds with all new source files
2. **Header isolation:** `BasicGame.cpp` compiles with zero GTE includes — verify no transitive GTE headers leak through AQUA public headers
3. **BasicGame runs:** displays a dark gray cleared window (no crash) using `scene->render(*this)`
4. **Pipeline test:** `app.createPipeline("test.omegasl", desc)` returns non-null
5. **Object compiles:** Object.h/cpp compile with no GTE dependency
6. **Scene hierarchy:** add child object with parent, verify `cachedWorld = parentWorld * childLocal`
7. **Math correctness:** `Mat4::perspective` and `Mat4::lookAt` produce same results as GTE equivalents
