# AQUA Pipeline, Object, and Scene — Implementation Plan

## Context

BasicGame currently works directly with raw GTE command buffers, render pass descriptors, and pipeline state objects. The goal is to add three abstractions so users don't touch the rendering plumbing:

- **Pipeline** — wraps shader loading + `GERenderPipelineState` creation
- **Object** — renderable entity: GEMesh + Pipeline + transform
- **Scene** — hierarchical transform tree that renders all its Objects in one call

GEMesh does not exist yet (see `gte/docs/GEMesh-TextureAssets-Implementation-Plan.md`). Object forward-declares it; full rendering activates when GEMesh lands.

---

## New Files

| File | Purpose |
|------|---------|
| `aqua/include/aqua/Pipeline.h` | Pipeline public API |
| `aqua/include/aqua/Object.h` | Object public API |
| `aqua/include/aqua/Scene.h` | Scene public API |
| `aqua/src/pipeline/Pipeline.cpp` | Pipeline impl (shader loading, pipeline state creation) |
| `aqua/src/Object.cpp` | Object impl (simple getters/setters) |
| `aqua/src/Scene.cpp` | Scene impl (hierarchy, transform propagation, render loop) |

## Modified Files

| File | Change |
|------|--------|
| `aqua/CMakeLists.txt` | Add 3 new source files to SOURCES |
| `aqua/tests/BasicGame.cpp` | Rewrite to use Pipeline + Scene (clear-only until GEMesh) |

---

## 1. Pipeline

**Header:** `aqua/include/aqua/Pipeline.h`

```
PipelineDesc
├── cullMode          (RasterCullMode, default Back)
├── fillMode          (TriangleFillMode, default Solid)
├── frontFace         (GTEPolygonFrontFaceRotation, default CounterClockwise)
├── enableDepth       (bool, default false)
├── vertexFunction    (string — name in shader library)
└── fragmentFunction  (string — name in shader library)

Pipeline
├── create(gte, omegaslPath, desc)        → shared_ptr<Pipeline>   [runtime compile]
├── createFromLibrary(gte, libPath, desc) → shared_ptr<Pipeline>   [pre-compiled]
├── renderPipelineState()                 → SharedHandle<GERenderPipelineState>&
└── shaderLibrary()                       → SharedHandle<GTEShaderLibrary>&
```

**Impl holds:** `GTEShaderLibrary`, `GERenderPipelineState`, retained `PipelineDesc`.

**`create()` flow:**
1. Read `.omegasl` source from file
2. `gte.omegaSlCompiler->compile(...)` → `omegasl_shader_lib` (guarded by `#if RUNTIME_SHADER_COMP_SUPPORT`)
3. `gte.graphicsEngine->loadShaderLibraryRuntime(lib)` → `GTEShaderLibrary`
4. Look up vertex/fragment shaders by name from `shaderLib->shaders`
5. Fill `RenderPipelineDescriptor` from `PipelineDesc` fields + shader handles
6. `gte.graphicsEngine->makeRenderPipelineState(rpDesc)`

**`createFromLibrary()` flow:** Same as above but step 2-3 replaced by `gte.graphicsEngine->loadShaderLibrary(path)`.

---

## 2. Object

**Header:** `aqua/include/aqua/Object.h`

Forward-declares `OmegaGTE::GEMesh`. Replaced by `#include <omegaGTE/GEMesh.h>` when GEMesh lands.

```
Object (enable_shared_from_this)
├── create(mesh, pipeline)  → shared_ptr<Object>
├── setTransform / transform()      FMatrix<4,4> (local space, default Identity)
├── setMesh / mesh()                SharedHandle<GEMesh>
├── setPipeline / pipeline()        shared_ptr<Pipeline>
├── setVisible / isVisible()        bool (default true)
└── setName / name()                string (debug)
```

**Impl holds:** mesh, pipeline, localTransform, visible, name. Pure data — no GPU commands.

---

## 3. Scene

**Header:** `aqua/include/aqua/Scene.h`

```
Scene
├── create()                                   → shared_ptr<Scene>
├── add(object, parent=nullptr)                hierarchy insert
├── remove(object)                             hierarchy remove
├── setViewMatrix(mat)                         camera view
├── setProjectionMatrix(mat)                   camera projection
├── setClearColor(r, g, b, a)                  background
└── render(renderTarget)                       encode + submit + present
```

**Internal scene graph:** Flat `std::vector<Node>` where each Node is `{shared_ptr<Object>, shared_ptr<Object> parent, FMatrix<4,4> cachedWorld}`.

**`render()` flow:**
1. Compute world transforms: walk nodes, `world = parent ? parentWorld * local : local`
2. `auto cmdBuf = renderTarget->commandBuffer()`
3. Start render pass with clear color
4. For each visible object (sorted by pipeline ptr to minimize state switches):
   - `cmdBuf->setRenderPipelineState(obj->pipeline()->renderPipelineState())`
   - Compute MVP = projection * view * world
   - Write MVP to per-object uniform buffer via `GEBufferWriter`
   - Bind vertex buffer from `mesh->vertexBuffer` at slot 0
   - Bind MVP uniform buffer at slot 1
   - Bind mesh textures at fragment slots
   - `cmdBuf->drawPolygons(Triangle, mesh->vertexCount, 0)`
5. End render pass
6. Submit + commitAndPresent

**Note:** Steps 4c-4f depend on GEMesh. Until it exists, `render()` only performs the clear pass (steps 2-3, 5-6). This still lets BasicGame display a cleared window using the Scene API.

---

## 4. CMakeLists.txt Change

```cmake
# aqua/CMakeLists.txt — change SOURCES line:
add_omega_graphics_module(AQUA SHARED
    HEADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include
    SOURCES
        src/App.cpp
        src/pipeline/Pipeline.cpp
        src/Object.cpp
        src/Scene.cpp
        ${PLATFORM_SRCS}
    DEPENDS OmegaGTE
)
```

---

## 5. BasicGame.cpp After

```cpp
class BasicGame : public Aqua::App {
    std::shared_ptr<Aqua::Scene> scene;
public:
    BasicGame() : App({{.title = "AQUA - BasicGame", .width = 1280, .height = 720}}) {}

    void onInit() override {
        scene = Aqua::Scene::create();
        scene->setClearColor(0.1f, 0.1f, 0.1f);
        scene->setProjectionMatrix(
            OmegaGTE::perspectiveProjection(
                OmegaGTE::Deg2Rad<float> * 60.f, 1280.f/720.f, 0.1f, 100.f));
        scene->setViewMatrix(
            OmegaGTE::lookAt({0,2,5}, {0,0,0}, {0,1,0}));
    }

    void onFrame() override {
        scene->render(renderTarget());
    }
};
```

Objects with meshes added once GEMesh is implemented.

---

## 6. Implementation Order

1. **Pipeline** — can be built and tested today (no GEMesh dependency)
2. **Object** — compiles today with forward-declared GEMesh; getters/setters only
3. **Scene** — hierarchy + clear-only render loop; mesh drawing activates when GEMesh lands
4. **Integration** — once GEMesh is in GTE, replace forward decl, implement draw-mesh in Scene::render, update BasicGame

---

## 7. Verification

1. **Build:** `cmake --build` succeeds with the 3 new source files
2. **BasicGame runs:** displays a dark gray cleared window (no crash) using `scene->render()`
3. **Pipeline isolation test:** create a Pipeline from an `.omegasl` file, verify `renderPipelineState()` is non-null
4. **Object compiles:** even with GEMesh forward-declared, Object.h/cpp compile cleanly
5. **Hierarchy:** add child object with parent, verify `cachedWorldTransform = parentWorld * childLocal`
