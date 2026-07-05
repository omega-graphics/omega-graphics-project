#include "../GTETestEntryPoint.h"
#include "../GTETestWindow.h"

#include <omegaGTE/GTEDevice.h>
#include <omegaGTE/GECommandQueue.h>
#include <omegaGTE/GEPipeline.h>
#include <omegaGTE/GERenderTarget.h>
#include <omegaGTE/GTEMath.h>
#include <omegaGTE/GTEShader.h>

#include <iostream>
#include <cmath>
#include <cassert>

// GPUTessTest — shared, backend-neutral body (GTETestWindow-CrossBackend-Plan.md,
// Phase 4). Compares the triangulation engine's CPU and GPU tessellation paths
// for a single rect and reports pass/fail; it never presents anything, so it
// needs only a NativeRenderTargetDescriptor to build the TE context from, not
// an actual redraw. onReady runs the comparison synchronously and then calls
// RequestGTETestWindowClose with the pass/fail exit code — the window opens
// and closes itself with no user interaction, mirroring the original three
// per-backend copies (a DX hidden window, a windowless Metal CAMetalLayer, and
// a real GTK window), now unified onto one real (briefly visible) window.

static bool comparePt(OmegaGTE::GPoint3D a, OmegaGTE::GPoint3D b, float tol = 0.01f) {
    return std::fabs(a.x - b.x) < tol &&
           std::fabs(a.y - b.y) < tol &&
           std::fabs(a.z - b.z) < tol;
}

static OmegaGTE::GTE gte;
static SharedHandle<OmegaGTE::GECommandQueue> commandQueue;
static SharedHandle<OmegaGTE::GENativeRenderTarget> renderTarget;
static SharedHandle<OmegaGTE::OmegaTriangulationEngineContext> teCtx;

static int runComparison() {
    OmegaGTE::GEViewport vp{0, 0, 800, 600, 0, 1};

    auto colorVec = OmegaGTE::FVec<4>::Create();
    colorVec[0][0] = 1.0f;
    colorVec[1][0] = 0.0f;
    colorVec[2][0] = 0.0f;
    colorVec[3][0] = 1.0f;

    OmegaGTE::GRect rect{OmegaGTE::GPoint2D{100, 100}, 200, 150};
    auto tessParams = OmegaGTE::TETriangulationParams::Rect(rect);
    tessParams.addAttachment(OmegaGTE::TETriangulationParams::Attachment::makeColor(colorVec));

    auto cpuResult = teCtx->triangulateSync(tessParams, OmegaGTE::GTEPolygonFrontFaceRotation::Clockwise, &vp);
    std::cout << "CPU tessellation: " << cpuResult.totalVertexCount() << " vertices, "
              << cpuResult.mesh.vertexPolygons.size() << " polygons" << std::endl;

    auto gpuFuture = teCtx->triangulateOnGPU(tessParams, OmegaGTE::GTEPolygonFrontFaceRotation::Clockwise, &vp);
    auto gpuResult = gpuFuture.get();
    std::cout << "GPU tessellation: " << gpuResult.totalVertexCount() << " vertices, "
              << gpuResult.mesh.vertexPolygons.size() << " polygons" << std::endl;

    bool pass = true;

    if (cpuResult.totalVertexCount() != gpuResult.totalVertexCount()) {
        std::cerr << "FAIL: vertex count mismatch: CPU=" << cpuResult.totalVertexCount()
                  << " GPU=" << gpuResult.totalVertexCount() << std::endl;
        pass = false;
    }

    if (pass) {
        auto &cpuMesh = cpuResult.mesh;
        auto &gpuMesh = gpuResult.mesh;
        if (cpuMesh.vertexPolygons.size() != gpuMesh.vertexPolygons.size()) {
            std::cerr << "FAIL: polygon count mismatch" << std::endl;
            pass = false;
        }
        for (size_t p = 0; p < cpuMesh.vertexPolygons.size() && pass; p++) {
            auto &cp = cpuMesh.vertexPolygons[p];
            auto &gp = gpuMesh.vertexPolygons[p];
            if (!comparePt(cp.a.pt, gp.a.pt) ||
                !comparePt(cp.b.pt, gp.b.pt) ||
                !comparePt(cp.c.pt, gp.c.pt)) {
                std::cerr << "FAIL: vertex mismatch in polygon " << p << std::endl;
                std::cerr << "  CPU: a=(" << cp.a.pt.x << "," << cp.a.pt.y << ") "
                          << "b=(" << cp.b.pt.x << "," << cp.b.pt.y << ") "
                          << "c=(" << cp.c.pt.x << "," << cp.c.pt.y << ")" << std::endl;
                std::cerr << "  GPU: a=(" << gp.a.pt.x << "," << gp.a.pt.y << ") "
                          << "b=(" << gp.b.pt.x << "," << gp.b.pt.y << ") "
                          << "c=(" << gp.c.pt.x << "," << gp.c.pt.y << ")" << std::endl;
                pass = false;
            }
        }
    }

    if (pass) {
        std::cout << "PASS: GPU tessellation matches CPU tessellation for rect." << std::endl;
    } else {
        std::cout << "FAIL: GPU tessellation does not match CPU tessellation." << std::endl;
    }

    return pass ? 0 : 1;
}

GTE_TEST_ENTRY_POINT {
    (void)argc;

    gte = OmegaGTE::InitWithDefaultDevice();
    std::cout << "GTE Initialized" << std::endl;

    OmegaGTETests::GTETestWindowDescriptor desc;
    desc.title = "GTE GPUTessTest";
    desc.width = 800;
    desc.height = 600;

    OmegaGTETests::GTETestWindowDelegate del;

    del.onReady = [](const OmegaGTE::NativeRenderTargetDescriptor &nrt) {
        OmegaGTE::GECommandQueueDesc commandQueueDesc{};
        commandQueueDesc.maxBufferCount = 64;
        commandQueue = gte.graphicsEngine->makeCommandQueue(commandQueueDesc);
        renderTarget = gte.graphicsEngine->makeNativeRenderTarget(nrt, commandQueue);

        teCtx = gte.triangulationEngine->createTEContextFromNativeRenderTarget(renderTarget);
        assert(teCtx && "Failed to create TE context");

        int exitCode = runComparison();
        OmegaGTETests::RequestGTETestWindowClose(exitCode);
    };

    del.onClose = []() {
        teCtx.reset();
        renderTarget.reset();
        commandQueue.reset();
        OmegaGTE::Close(gte);
    };

    return OmegaGTETests::RunGTETestWindow(argc, argv, desc, del);
};
