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
// Layout mirrors GPUTessTest (simple WinMain, default device, single
// window, single render pass, no message-loop UI). FBX loading is a
// follow-up — the test draws a hardcoded meshlet today so the path is
// observable without needing assets.

#include <omegaGTE/GTEDevice.h>
#include <omegaGTE/GECommandQueue.h>
#include <omegaGTE/GEPipeline.h>
#include <omegaGTE/GERenderTarget.h>
#include <omegaGTE/GTEMath.h>
#include <omegaGTE/GTEShader.h>
#include <omegaGTE/GEMesh.h>
#include <omegaGTE/GEMeshAsset.h>

#include <omega-common/fs.h>

#include <windows.h>
#include <iostream>
#include <cassert>

#define MESH_SHADER     "meshFunc"
#define FRAGMENT_SHADER "fragFunc"

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam){
    if(msg == WM_DESTROY){
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/,
                     LPSTR /*lpCmdLine*/, int nShowCmd){
    auto gte = OmegaGTE::InitWithDefaultDevice();
    std::cout << "[MeshAndRaytracingTest] GTE Initialized" << std::endl;

    // Mesh-shader feature gate. The Phase 3 public API also checks this
    // inside `makeMeshPipelineState`, but reporting it up-front gives
    // the user a clearer one-line diagnostic than chasing the per-call
    // log later. Same shape we'll use for raytracing when it joins.
    auto enumerateRes = OmegaGTE::enumerateDevices();
    bool meshSupported = false;
    for(auto &dev : enumerateRes){
        if(dev && dev->features.hasFeature(OmegaGTE::GTEDEVICE_FEATURE_MESH_SHADER)){
            meshSupported = true;
            break;
        }
    }
    std::cout << "[MeshAndRaytracingTest] GTEDEVICE_FEATURE_MESH_SHADER = "
              << (meshSupported ? "YES" : "NO") << std::endl;

    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "_MeshAndRaytracingTest";
    RegisterClassExA(&wc);

    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "MeshAndRaytracingTest",
                                WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,
                                nullptr, nullptr, hInstance, nullptr);
    assert(hwnd && "Failed to create window");

    OmegaGTE::NativeRenderTargetDescriptor rtDesc{};
    rtDesc.isHwnd = true;
    rtDesc.hwnd = hwnd;
    OmegaGTE::GECommandQueueDesc commandQueueDesc{};
    commandQueueDesc.maxBufferCount = 64;
    auto commandQueue = gte.graphicsEngine->makeCommandQueue(commandQueueDesc);
    auto renderTarget = gte.graphicsEngine->makeNativeRenderTarget(rtDesc, commandQueue);
    assert(renderTarget && "Failed to create native render target");

    // Shader library load. CMake stages the compiled `.omegasllib` next
    // to the test executable (see `add_d3d12_test`'s SHADERS handling),
    // so resolve the path against the exe directory rather than the
    // shell's cwd — otherwise running the test from a terminal opened
    // somewhere else (e.g. the repo root) makes `./meshAndRaytracing.omegasllib`
    // resolve to the wrong place. `FS::getExecutableDir()` returns the
    // absolute exe directory with no trailing separator so it composes
    // cleanly with `append` / `operator+`. Mesh shader itself is gated
    // on MESH_SHADERS at the OmegaSL `#requires(...)` layer — runtime
    // loader rejects cleanly on devices without the feature.
    auto shaderLibPath = OmegaCommon::FS::getExecutableDir()
                             .append("meshAndRaytracing.omegasllib");
    auto library = gte.graphicsEngine->loadShaderLibrary(shaderLibPath);
    assert(library && "Failed to load meshAndRaytracing.omegasllib");

    OmegaGTE::MeshPipelineDescriptor meshPipeDesc{};
    meshPipeDesc.name = "MeshAndRaytracing.Pipeline";
    meshPipeDesc.meshFunc     = library->shaders[MESH_SHADER];
    meshPipeDesc.fragmentFunc = library->shaders[FRAGMENT_SHADER];
    meshPipeDesc.depthAndStencilDesc.enableDepth = false;
    meshPipeDesc.depthAndStencilDesc.enableStencil = false;

    auto meshPipeline = gte.graphicsEngine->makeMeshPipelineState(meshPipeDesc);
    const bool meshPipelineLive = (meshPipeline != nullptr);
    std::cout << "[MeshAndRaytracingTest] makeMeshPipelineState -> "
              << (meshPipelineLive ? "live PSO" : "nullptr")
              << std::endl;

    // ── Load the FBX through GEMeshAsset ────────────────────────────
    // CMake stages `orange_tennis_racket.fbx` next to the executable
    // via `add_d3d12_test`'s ASSETS list (see CMakeLists.txt). The
    // mesh-asset loader supports a simple FBX path on D3D12 today —
    // textures are skipped (`loadMaterialTextures = false`) so the
    // first integration stays focused on geometry.
    //
    // `desiredDescriptor.attributes = Position` keeps the per-vertex
    // stride at 12B (tightly packed `float3`). Adding Normal would
    // jump the GEMesh CPU stride to 24B while OmegaSL `buffer<T>` on
    // D3D12 would pad to 32B (std430 16-byte vec3 alignment between
    // consecutive vec3 fields) — they wouldn't agree, and the shader
    // would read garbage. See the matching note in the shader file.
    auto meshAsset = OmegaGTE::GEMeshAsset::Create(gte.graphicsEngine);
    OmegaGTE::GEMeshAsset::LoadOptions meshOpts{};
    meshOpts.desiredDescriptor.attributes = OmegaGTE::GEMeshAttrPosition;
    meshOpts.desiredDescriptor.topology   = OmegaGTE::GEMeshTopology::Triangle;
    meshOpts.desiredDescriptor.indexType  = OmegaGTE::GEMeshIndexType::None;
    meshOpts.loadMaterialTextures = false;
    auto fbxPath = OmegaCommon::FS::getExecutableDir()
                       .append("orange_tennis_racket.fbx");
    const bool meshLoaded = meshAsset->load(fbxPath.absPath(), meshOpts);
    SharedHandle<OmegaGTE::GEMesh> loadedMesh =
        meshLoaded ? meshAsset->mesh() : nullptr;
    if(meshLoaded && loadedMesh){
        std::cout << "[MeshAndRaytracingTest] FBX loaded: vertexCount="
                  << loadedMesh->vertexCount
                  << " stride=" << loadedMesh->vertexStride << "B ("
                  << (loadedMesh->vertexCount / 3u) << " triangles)" << std::endl;
    } else {
        std::cerr << "[MeshAndRaytracingTest] FBX load failed; dispatching "
                     "a single empty mesh threadgroup to exercise the "
                     "pipeline anyway." << std::endl;
    }

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

    if(meshPipelineLive){
        commandBuffer->setRenderPipelineState(meshPipeline);
        if(loadedMesh && loadedMesh->vertexBuffer && loadedMesh->vertexCount >= 3){
            // The mesh shader stores its handle in the
            // GED3D12RenderPipelineState `vertexShader` slot (per
            // Phase 4b.1), and the resource-binding paths read from
            // that slot regardless of whether the bound stage is
            // actually vertex or mesh — so the existing
            // `bindResourceAtVertexShader` is the correct bind hook
            // for a mesh-shader buffer too. Register 0 matches the
            // `: 0` annotation on `buffer<VertexIn>` in the shader.
            commandBuffer->bindResourceAtVertexShader(loadedMesh->vertexBuffer, 0);
            // One mesh threadgroup per triangle. `tid.x` in the shader
            // is the triangle index; the shader pulls 3 consecutive
            // vertices from the bound buffer. Real meshlet
            // partitioning (64+ verts per threadgroup) is a Phase 6
            // follow-up that needs a meshlet builder.
            uint32_t triCount = loadedMesh->vertexCount / 3u;
            commandBuffer->drawMeshTasks(triCount, 1, 1);
        } else {
            // FBX load failed or returned no geometry. Dispatch a
            // single threadgroup so the pipeline runs end-to-end and
            // we observe the slate-grey clear without a crash.
            commandBuffer->drawMeshTasks(1, 1, 1);
        }
    } else {
        commandBuffer->drawMeshTasks(1, 1, 1);
    }

    commandBuffer->finishRenderPass();
    commandQueue->submitCommandBuffer(commandBuffer);
    commandQueue->commitToGPU();
    renderTarget->present();

    ShowWindow(hwnd, nShowCmd);
    UpdateWindow(hwnd);

    // ── FBX loading + raytracing hookup ──────────────────────────────
    // TODO(MeshAndRaytracing-Phase2): swap the hardcoded shader meshlet
    // for a real FBX mesh via GEMesh (mesh asset import is a separate
    // surface). The mesh pipeline's vertex output struct stays the
    // same; only the source of the per-meshlet data changes.
    //
    // TODO(MeshAndRaytracing-Phase2): when raytracing reaches the
    // dispatch surface, build a TLAS over the same geometry and
    // dispatch a raygen pass into a fullscreen texture, then blit it
    // into the same window as a second pass.

    MSG msg{};
    while(msg.message != WM_QUIT){
        if(PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)){
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }

    DestroyWindow(hwnd);
    UnregisterClassA(wc.lpszClassName, hInstance);
    OmegaGTE::Close(gte);
    return 0;
}
