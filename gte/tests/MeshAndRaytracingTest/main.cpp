#include "../GTETestEntryPoint.h"
#include "../GTETestWindow.h"

// MeshAndRaytracingTest — visual test for the mesh-shader pipeline driving a
// GEMeshAsset-loaded FBX, placed in the scene by GESpace.
//
// The mesh shader consumes the GEMesh's vertex buffer directly (one mesh
// threadgroup per triangle) and transforms each vertex by a single push-constant
// MVP. That matrix is `GESpace::objectTransform()` — GESpace-Implementation-Plan
// Phase 3. The point of routing through GESpace rather than hand-writing a matrix
// here: the GEMesh's vertex buffer stays in the FBX's own local units (GESpace
// never re-bakes geometry on the CPU), and the space owns the one conversion from
// those units to NDC.
//
// Before GESpace this test handed the raw FBX coordinates to the rasterizer as if
// they were already clip-space — so the racket, authored in units that span
// hundreds, landed almost entirely outside the [-1,1] clip volume and the window
// showed nothing but the clear color.
//
// Shared, backend-neutral body (GTETestWindow-CrossBackend-Plan.md, Phase 4);
// runs today on D3D12 and Vulkan, wherever GTEDEVICE_FEATURE_MESH_SHADER is
// present.
//
// TODO: there is no depth attachment on the test window's render target, so the
// draw has no depth test — 389k triangles resolve in dispatch order and back
// faces can paint over front ones. The silhouette and placement (what this test
// is verifying) are correct regardless; proper occlusion needs a depth buffer on
// the native render target, which is a separate piece of work.

#include <omegaGTE/GTEDevice.h>
#include <omegaGTE/GECommandQueue.h>
#include <omegaGTE/GEPipeline.h>
#include <omegaGTE/GERenderTarget.h>
#include <omegaGTE/GTEMath.h>
#include <omegaGTE/GTEShader.h>
#include <omegaGTE/GEMesh.h>
#include <omegaGTE/GEMeshAsset.h>
#include <omegaGTE/GESpace.h>

#include <omega-common/fs.h>

#include <algorithm>
#include <iostream>
#include <memory>

#define AMPLIFICATION_SHADER "ampFunc"
#define MESH_SHADER          "meshFunc"
#define FRAGMENT_SHADER      "fragFunc"

// Triangles per amplification threadgroup. Must match the `32u` in the shader's
// `ampFunc` — the two together define the batch, and a disagreement would either
// skip triangles (host batch larger) or run mesh groups past the end of the
// vertex buffer (host batch smaller).
static constexpr uint32_t kTrianglesPerBatch = 32;

// Window / viewport extent, in pixels. These are also the space's X/Y units:
// GESpace maps space units → NDC straight through the viewport, so authoring in
// pixels means "translate to (400, 300)" puts an object in the middle of an
// 800x600 window.
static constexpr float kWindowWidth  = 800.f;
static constexpr float kWindowHeight = 600.f;

// Depth range of the SPACE, in space units — NOT the rasterizer viewport's
// [0,1] depth range, which is a different thing that happens to live on the same
// struct. The fit below scales the model to hundreds of space units on its
// longest axis, so the space needs a depth range on that same order or the mesh
// is scaled straight through the far plane and clipped away entirely (with every
// matrix still "correct"). Symmetric about 0, so a model centered at z=0 sits at
// NDC depth 0.5 with room on both sides to rotate.
static constexpr float kSpaceNearDepth = -1000.f;
static constexpr float kSpaceFarDepth  =  1000.f;

// Fraction of the viewport's shorter axis the fitted model should span.
static constexpr float kFillFraction = 0.8f;

/// The `constant<MeshTransform>` block, declared `[in pc]` by BOTH the
/// amplification and the mesh stage: one column-major float4x4 plus the triangle
/// count.
///
/// `triCount` is what the amplification stage clamps its tail batch against. It
/// lives in the push constant rather than being baked into the dispatch because
/// the amp shader — not the host — is the thing that has to know when to stop.
///
/// Layout is std430: the mat4 is 64B at offset 0 (16B-aligned), the uint is 4B
/// at offset 64. The C++ struct below matches byte for byte with no padding
/// (alignof(float[16]) == alignof(uint32_t) == 4), which is why this can be
/// memcpy'd straight into the push-constant range.
struct MeshTransformConstants {
    float    mvp[16];
    uint32_t triCount;
};

static OmegaGTE::GTE gte;
static SharedHandle<OmegaGTE::GTEShaderLibrary> library;
static SharedHandle<OmegaGTE::GERenderPipelineState> meshPipeline;
static SharedHandle<OmegaGTE::GEMeshAsset> meshAsset;
static SharedHandle<OmegaGTE::GEMesh> loadedMesh;
static SharedHandle<OmegaGTE::GECommandQueue> commandQueue;
static SharedHandle<OmegaGTE::GENativeRenderTarget> renderTarget;

// The coordinate space the mesh is placed in, and the handle it was placed at.
static UniqueHandle<OmegaGTE::GESpace> space;
static OmegaGTE::GESpaceObjectID meshObject = OmegaGTE::GESpaceInvalidObject;

/// Flatten an FMatrix<4,4> into the 16 floats the shader's `float4x4` expects.
/// FMatrix stores column-major (`m[col][row]`, one whole column contiguous), and
/// so does the shader's matrix, so column `c` goes to floats [4c .. 4c+3] — no
/// transpose. Spelled out as a loop rather than a memcpy of the private storage
/// because getting this backwards is silent: a transposed MVP still produces a
/// plausible-looking picture, just the wrong one.
static MeshTransformConstants flattenColumnMajor(const OmegaGTE::FMatrix<4,4> &m,
                                                 uint32_t triCount){
    MeshTransformConstants out{};
    for (unsigned c = 0; c < 4; ++c) {
        for (unsigned r = 0; r < 4; ++r) {
            out.mvp[c * 4 + r] = m[c][r];
        }
    }
    out.triCount = triCount;
    return out;
}

/// Place `mesh` in `space` so it lands centered in the viewport and spans
/// `kFillFraction` of its shorter axis, whatever units the source file used.
///
/// This is the whole reason `GEMesh::bounds` exists: the vertex buffer is a GPU
/// resource with no CPU copy, so without the local-space AABB captured at load
/// time the scale here would be a magic number tuned by eye to one asset. The
/// model is centered on all three axes — including Z, where an off-origin model
/// scaled by a few hundred would otherwise be pushed clean through the far plane.
static OmegaGTE::GESpaceObjectID fitMeshToSpace(OmegaGTE::GESpace &sp,
                                                const SharedHandle<OmegaGTE::GEMesh> &mesh){
    const auto id = sp.addMesh(mesh);
    if (id == OmegaGTE::GESpaceInvalidObject) {
        return id;
    }

    const auto &bounds = mesh->bounds;
    if (!bounds.valid || bounds.longestExtent() <= 0.f) {
        // A mesh with no bounds (empty, or loaded by a path that does not
        // populate them) can still be placed — it just cannot be auto-fitted, so
        // say so instead of dividing by zero and drawing a NaN.
        std::cerr << "[MeshAndRaytracingTest] warning: mesh has no valid bounds; "
                     "placing it untransformed (it will almost certainly be off-screen)."
                  << std::endl;
        return id;
    }

    const float fit = kFillFraction * std::min(kWindowWidth, kWindowHeight)
                    / bounds.longestExtent();
    const auto center = bounds.center();

    sp.setScale(id, OmegaGTE::GPoint3D{fit, fit, fit});
    // Scale is applied before translation (GESpaceTransform is T∘R∘S), so the
    // model's own center must be pre-scaled here to cancel it.
    sp.setTranslation(id, OmegaGTE::GPoint3D{kWindowWidth  * 0.5f - center.x * fit,
                                             kWindowHeight * 0.5f - center.y * fit,
                                             0.f                  - center.z * fit});

    std::cout << "[MeshAndRaytracingTest] mesh bounds (local): min("
              << bounds.min.x << ", " << bounds.min.y << ", " << bounds.min.z << ") max("
              << bounds.max.x << ", " << bounds.max.y << ", " << bounds.max.z << ")"
              << "\n[MeshAndRaytracingTest] fit scale = " << fit
              << " (longest extent " << bounds.longestExtent() << " space units -> "
              << (kFillFraction * std::min(kWindowWidth, kWindowHeight)) << " px)"
              << std::endl;
    return id;
}

GTE_TEST_ENTRY_POINT {
    (void)argc;

    // Point the working directory at the executable's folder so the relative
    // "./meshAndRaytracing.omegasllib" / "./orange_tennis_racket.fbx" loads
    // below resolve regardless of where the test is launched from — same
    // convention 2DTest uses.
    OmegaCommon::FS::changeCWD(OmegaCommon::FS::getExecutableDir());

    gte = OmegaGTE::InitWithDefaultDevice();
    std::cout << "[MeshAndRaytracingTest] GTE Initialized" << std::endl;

    // Mesh-shader feature gate. The Phase 3 public API also checks this
    // inside `makeMeshPipelineState`, but reporting it up-front gives the
    // user a clearer one-line diagnostic than chasing the per-call log
    // later. Same shape we'll use for raytracing when it joins.
    auto enumerateRes = OmegaGTE::enumerateDevices();
    bool meshSupported = false;
    for (auto &dev : enumerateRes) {
        if (dev && dev->features.hasFeature(OmegaGTE::GTEDEVICE_FEATURE_MESH_SHADER)) {
            meshSupported = true;
            break;
        }
    }
    std::cout << "[MeshAndRaytracingTest] GTEDEVICE_FEATURE_MESH_SHADER = "
              << (meshSupported ? "YES" : "NO") << std::endl;

    library = gte.graphicsEngine->loadShaderLibrary("./meshAndRaytracing.omegasllib");
    if (!library) {
        std::cerr << "[MeshAndRaytracingTest] failed to load ./meshAndRaytracing.omegasllib" << std::endl;
        return 1;
    }

    // Two-stage mesh pipeline: amplification -> mesh -> fragment (§5). The
    // amplification stage batches the model's triangles 32 at a time and
    // dispatches the mesh threadgroups for each batch itself, so the host issues
    // one dispatch per BATCH instead of one per triangle.
    OmegaGTE::MeshPipelineDescriptor meshPipeDesc{};
    meshPipeDesc.name = "MeshAndRaytracing.Pipeline";
    meshPipeDesc.amplificationFunc = library->shaders[AMPLIFICATION_SHADER];
    meshPipeDesc.meshFunc          = library->shaders[MESH_SHADER];
    meshPipeDesc.fragmentFunc      = library->shaders[FRAGMENT_SHADER];
    meshPipeDesc.depthAndStencilDesc.enableDepth = false;
    meshPipeDesc.depthAndStencilDesc.enableStencil = false;

    meshPipeline = gte.graphicsEngine->makeMeshPipelineState(meshPipeDesc);
    const bool meshPipelineLive = (meshPipeline != nullptr);
    std::cout << "[MeshAndRaytracingTest] makeMeshPipelineState (amplification + mesh) -> "
              << (meshPipelineLive ? "live PSO" : "nullptr")
              << std::endl;

    // ── Load the FBX through GEMeshAsset ────────────────────────────
    // `desiredDescriptor.attributes = Position` keeps the per-vertex
    // stride at 12B (tightly packed `float3`). Adding Normal would jump
    // the GEMesh CPU stride to 24B while OmegaSL `buffer<T>` on D3D12
    // would pad to 32B (std430 16-byte vec3 alignment between
    // consecutive vec3 fields) — they wouldn't agree, and the shader
    // would read garbage. See the matching note in the shader file.
    meshAsset = OmegaGTE::GEMeshAsset::Create(gte.graphicsEngine);
    OmegaGTE::GEMeshAsset::LoadOptions meshOpts{};
    meshOpts.desiredDescriptor.attributes = OmegaGTE::GEMeshAttrPosition;
    meshOpts.desiredDescriptor.topology   = OmegaGTE::GEMeshTopology::Triangle;
    meshOpts.desiredDescriptor.indexType  = OmegaGTE::GEMeshIndexType::None;
    meshOpts.loadMaterialTextures = false;
    const bool meshLoaded = meshAsset->load("./orange_tennis_racket.fbx", meshOpts);
    loadedMesh = meshLoaded ? meshAsset->mesh() : nullptr;
    if (meshLoaded && loadedMesh) {
        std::cout << "[MeshAndRaytracingTest] FBX loaded: vertexCount="
                  << loadedMesh->vertexCount
                  << " stride=" << loadedMesh->vertexStride << "B ("
                  << (loadedMesh->vertexCount / 3u) << " triangles)" << std::endl;
    } else {
        std::cerr << "[MeshAndRaytracingTest] FBX load failed; dispatching "
                     "a single empty mesh threadgroup to exercise the "
                     "pipeline anyway." << std::endl;
    }

    // ── Place the mesh in a GESpace ─────────────────────────────────
    // The space is defined by a viewport: X/Y in pixels (so the space's units
    // ARE screen units) and a depth range in those same space units. GESpace
    // owns the one conversion from here to NDC; the mesh's vertex buffer is
    // never touched.
    OmegaGTE::GEViewport spaceViewport;
    spaceViewport.x         = 0.f;
    spaceViewport.y         = 0.f;
    spaceViewport.width     = kWindowWidth;
    spaceViewport.height    = kWindowHeight;
    spaceViewport.nearDepth = kSpaceNearDepth;
    spaceViewport.farDepth  = kSpaceFarDepth;

    space = std::make_unique<OmegaGTE::GESpace>(spaceViewport);
    if (loadedMesh) {
        // No reorientation: the asset's measured bounds (~1.90 x 0.81 x 0.17)
        // say it already lies flat in the XY plane — Z is its thin axis — so it
        // faces the viewer as authored. Rotating it upright, as a Z-up model
        // would need, would turn it edge-on and show a sliver. The bounds are
        // what make that knowable instead of a guess.
        meshObject = fitMeshToSpace(*space, loadedMesh);
    }

    OmegaGTETests::GTETestWindowDescriptor desc;
    desc.title = "GTE MeshAndRaytracingTest";
    desc.width = static_cast<unsigned>(kWindowWidth);
    desc.height = static_cast<unsigned>(kWindowHeight);

    OmegaGTETests::GTETestWindowDelegate del;

    del.onReady = [meshPipelineLive](const OmegaGTE::NativeRenderTargetDescriptor &nrt) {
        OmegaGTE::GECommandQueueDesc commandQueueDesc{};
        commandQueueDesc.maxBufferCount = 64;
        commandQueue = gte.graphicsEngine->makeCommandQueue(commandQueueDesc);
        renderTarget = gte.graphicsEngine->makeNativeRenderTarget(nrt, commandQueue);

        auto commandBuffer = commandQueue->getAvailableBuffer();

        OmegaGTE::GERenderPassDescriptor renderPassDesc{};
        renderPassDesc.nRenderTarget = renderTarget.get();
        using ColorAttachment = OmegaGTE::GERenderPassDescriptor::ColorAttachment;
        // Slate-grey clear so a stub run still shows "I drew something" without
        // false-positiving the meshlet (the triangle uses pure red/green/blue
        // vertex colors that contrast hard against grey).
        renderPassDesc.colorAttachments.push_back(
            ColorAttachment(ColorAttachment::ClearColor(0.15f, 0.15f, 0.18f, 1.f),
                            ColorAttachment::Clear));

        // The RASTERIZER viewport. Its nearDepth/farDepth are the hardware depth
        // range and must stay within [0,1] — unlike the space viewport above,
        // whose depth range is in space units. Same struct, two different jobs.
        OmegaGTE::GEViewport    viewport    {0, 0, kWindowWidth, kWindowHeight, 0.f, 1.f};
        OmegaGTE::GEScissorRect scissorRect {0, 0, kWindowWidth, kWindowHeight};

        commandBuffer->startRenderPass(renderPassDesc);
        commandBuffer->setViewports({viewport});
        commandBuffer->setScissorRects({scissorRect});

        if (meshPipelineLive) {
            commandBuffer->setRenderPipelineState(meshPipeline);
            if (loadedMesh && loadedMesh->vertexBuffer && loadedMesh->vertexCount >= 3
                && meshObject != OmegaGTE::GESpaceInvalidObject) {
                const uint32_t triCount = loadedMesh->vertexCount / 3u;

                // THE transform. `objectTransform` composes the object's TRS (in
                // space units) with the space's own space→NDC map, so this single
                // matrix carries an FBX-local vertex all the way to clip space.
                // The mesh's GPU buffer is never rewritten — the matrix does the
                // work, which is the entire matrix-only premise of GESpace.
                //
                // One call feeds BOTH stages: `pc` is declared `[in pc]` on the
                // amplification shader (which reads `triCount` to clamp its tail
                // batch) and on the mesh shader (which reads `mvp`). Each backend
                // fans the write out to every stage that declared the block —
                // Vulkan via the push-constant range's stage mask, D3D12 via a
                // root-constants param per stage, Metal via
                // setObjectBytes/setMeshBytes.
                const auto constants =
                    flattenColumnMajor(space->objectTransform(meshObject), triCount);
                commandBuffer->setRenderConstants(&constants, sizeof(constants));

                // The mesh shader sits in the pipeline's `vertexShader` slot (the
                // slot-doubling from Phase 4a/4b/4c — mesh REPLACES vertex), so
                // `bindResourceAtVertexShader` is the correct bind hook for a
                // mesh-stage buffer. Register 1 matches the `: 1` annotation on
                // `buffer<VertexIn>` in the shader (slot 0 is the push-constant
                // block).
                //
                // The amplification stage reads no buffers here, so it needs no
                // `bindResourceAtAmplificationShader` — its only input is the push
                // constant above. If it did (a real culler wants the geometry), it
                // is a genuinely separate stage with its own descriptor set /
                // register space, which is why that bind is its own method rather
                // than a routing branch inside this one.
                commandBuffer->bindResourceAtVertexShader(loadedMesh->vertexBuffer, 1);

                // ONE amplification threadgroup per 32-triangle batch — not one
                // dispatch per triangle. Each amp group then launches its own mesh
                // children (32, or fewer for the tail). The dispatch count the host
                // issues therefore drops by 32x, and the GPU decides the rest.
                const uint32_t batchCount =
                    (triCount + kTrianglesPerBatch - 1u) / kTrianglesPerBatch;
                std::cout << "[MeshAndRaytracingTest] dispatching " << batchCount
                          << " amplification threadgroups for " << triCount
                          << " triangles (" << kTrianglesPerBatch << " per batch)"
                          << std::endl;
                commandBuffer->drawMeshTasks(batchCount, 1, 1);
            } else {
                // FBX load failed or returned no geometry. Still run the pipeline
                // end-to-end (so a broken PSO surfaces as a crash/validation error
                // rather than as a silently empty window) but with triCount = 0,
                // which makes the amplification stage dispatch zero mesh children.
                // A zero-count `dispatchMesh` is legal on all three backends.
                MeshTransformConstants constants{};  // zeroed: mvp = 0, triCount = 0
                commandBuffer->setRenderConstants(&constants, sizeof(constants));
                commandBuffer->drawMeshTasks(1, 1, 1);
            }
        }
        // No `else` here: with no live PSO there is nothing bound to dispatch
        // against, and `drawMeshTasks` now asserts on that (it did not when this
        // test was written against the Phase-3 stubs, which no-op'd).

        commandBuffer->finishRenderPass();
        commandQueue->submitCommandBuffer(commandBuffer);
        commandQueue->commitToGPU();
        renderTarget->present();
    };

    del.onClose = []() {
        commandQueue->commitToGPUAndWait();

        // The space holds a reference to the mesh; drop it before the mesh so
        // the GPU buffer is released once, in a defined order.
        space.reset();
        loadedMesh.reset();
        meshAsset.reset();
        meshPipeline.reset();
        renderTarget.reset();
        library.reset();
        commandQueue.reset();

        OmegaGTE::Close(gte);
    };

    // TODO(MeshAndRaytracing-Phase2): when raytracing reaches the dispatch
    // surface, build a TLAS over the same geometry and dispatch a raygen pass
    // into a fullscreen texture, then blit it into the same window as a
    // second pass.

    return OmegaGTETests::RunGTETestWindow(argc, argv, desc, del);
};
