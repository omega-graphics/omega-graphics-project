#include "../GTETestEntryPoint.h"
#include "../GTETestWindow.h"

// MeshAndRaytracingTest — D3D12 visual test for the mesh-shader pipeline
// (Phase 3 public API + Phase 4b dispatch) and, when it lands, the
// raytracing pipeline as well. Today the C++ wiring goes through the
// Phase-3 stubs in `makeMeshPipelineState` / `drawMeshTasks`, which
// log a precise "not yet implemented" diagnostic and degrade to a
// clear-screen-only render. Once Phase 4b lands the meshlet defined
// in `shaders.omegasl` (a single colorful triangle) appears on the
// child window. Raytracing hooks will be added when that phase
// reaches the dispatch surface.
//
// Shared, backend-neutral body (GTETestWindow-CrossBackend-Plan.md, Phase 4).
// D3D12 only today — mesh shaders have not landed on Metal/Vulkan yet — but
// the body is written against the portable RunGTETestWindow surface so the
// other backends only need their own CMake wiring when that day comes.

#include <omegaGTE/GTEDevice.h>
#include <omegaGTE/GECommandQueue.h>
#include <omegaGTE/GEPipeline.h>
#include <omegaGTE/GERenderTarget.h>
#include <omegaGTE/GTEMath.h>
#include <omegaGTE/GTEShader.h>
#include <omegaGTE/GEMesh.h>
#include <omegaGTE/GEMeshAsset.h>

#include <omega-common/fs.h>

#include <iostream>

#define MESH_SHADER     "meshFunc"
#define FRAGMENT_SHADER "fragFunc"

static OmegaGTE::GTE gte;
static SharedHandle<OmegaGTE::GTEShaderLibrary> library;
static SharedHandle<OmegaGTE::GERenderPipelineState> meshPipeline;
static SharedHandle<OmegaGTE::GEMeshAsset> meshAsset;
static SharedHandle<OmegaGTE::GEMesh> loadedMesh;
static SharedHandle<OmegaGTE::GECommandQueue> commandQueue;
static SharedHandle<OmegaGTE::GENativeRenderTarget> renderTarget;

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

    OmegaGTE::MeshPipelineDescriptor meshPipeDesc{};
    meshPipeDesc.name = "MeshAndRaytracing.Pipeline";
    meshPipeDesc.meshFunc     = library->shaders[MESH_SHADER];
    meshPipeDesc.fragmentFunc = library->shaders[FRAGMENT_SHADER];
    meshPipeDesc.depthAndStencilDesc.enableDepth = false;
    meshPipeDesc.depthAndStencilDesc.enableStencil = false;

    meshPipeline = gte.graphicsEngine->makeMeshPipelineState(meshPipeDesc);
    const bool meshPipelineLive = (meshPipeline != nullptr);
    std::cout << "[MeshAndRaytracingTest] makeMeshPipelineState -> "
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

    OmegaGTETests::GTETestWindowDescriptor desc;
    desc.title = "GTE MeshAndRaytracingTest";
    desc.width = 800;
    desc.height = 600;

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

        OmegaGTE::GEViewport    viewport    {0, 0, 800, 600, 0, 1.f};
        OmegaGTE::GEScissorRect scissorRect {0, 0, 800, 600};

        commandBuffer->startRenderPass(renderPassDesc);
        commandBuffer->setViewports({viewport});
        commandBuffer->setScissorRects({scissorRect});

        if (meshPipelineLive) {
            commandBuffer->setRenderPipelineState(meshPipeline);
            if (loadedMesh && loadedMesh->vertexBuffer && loadedMesh->vertexCount >= 3) {
                // The mesh shader stores its handle in the GED3D12RenderPipeline-
                // State `vertexShader` slot (per Phase 4b.1), and the resource-
                // binding paths read from that slot regardless of whether the
                // bound stage is actually vertex or mesh — so the existing
                // `bindResourceAtVertexShader` is the correct bind hook for a
                // mesh-shader buffer too. Register 0 matches the `: 0`
                // annotation on `buffer<VertexIn>` in the shader.
                commandBuffer->bindResourceAtVertexShader(loadedMesh->vertexBuffer, 0);
                // One mesh threadgroup per triangle. `tid.x` in the shader is
                // the triangle index; the shader pulls 3 consecutive vertices
                // from the bound buffer. Real meshlet partitioning (64+ verts
                // per threadgroup) is a follow-up that needs a meshlet builder.
                uint32_t triCount = loadedMesh->vertexCount / 3u;
                commandBuffer->drawMeshTasks(triCount, 1, 1);
            } else {
                // FBX load failed or returned no geometry. Dispatch a single
                // threadgroup so the pipeline runs end-to-end and we observe
                // the slate-grey clear without a crash.
                commandBuffer->drawMeshTasks(1, 1, 1);
            }
        } else {
            commandBuffer->drawMeshTasks(1, 1, 1);
        }

        commandBuffer->finishRenderPass();
        commandQueue->submitCommandBuffer(commandBuffer);
        commandQueue->commitToGPU();
        renderTarget->present();
    };

    del.onClose = []() {
        commandQueue->commitToGPUAndWait();

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
