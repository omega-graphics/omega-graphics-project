#include "../GTETestEntryPoint.h"
#include "../GTETestWindow.h"

#include <omegaGTE/GTEDevice.h>
#include <omegaGTE/GECommandQueue.h>
#include <omegaGTE/GEPipeline.h>
#include <omegaGTE/GERenderTarget.h>
#include <omegaGTE/GTEMath.h>
#include <omegaGTE/GTEShader.h>

#include <iostream>
#include <cassert>

// CPUTessTest — shared, backend-neutral body (GTETestWindow-CrossBackend-
// Plan.md, Phase 4). Exercises the CPU triangulation path against a pyramid,
// a cylinder, a cone, and a GVectorPath3D; renders nothing, so it needs only
// a NativeRenderTargetDescriptor to build the TE context from. onReady runs
// the checks synchronously and calls RequestGTETestWindowClose with the
// pass/fail exit code — no user interaction needed. Vulkan-only today.

static OmegaGTE::GTE gte;
static SharedHandle<OmegaGTE::GECommandQueue> commandQueue;
static SharedHandle<OmegaGTE::GENativeRenderTarget> renderTarget;
static SharedHandle<OmegaGTE::OmegaTriangulationEngineContext> teCtx;

static int runTests() {
    OmegaGTE::GEViewport vp{0, 0, 800, 600, 0, 1};
    bool allPassed = true;

    {
        std::cout << "\n=== Pyramid Tessellation ===" << std::endl;
        OmegaGTE::GPyramid pyramid{};
        pyramid.x = 0; pyramid.y = 0; pyramid.z = 0;
        pyramid.w = 100; pyramid.d = 100; pyramid.h = 150;
        auto params = OmegaGTE::TETriangulationParams::Pyramid(pyramid);
        auto result = teCtx->triangulateSync(params, OmegaGTE::GTEPolygonFrontFaceRotation::Clockwise, &vp);
        std::cout << "  Polygons: " << result.mesh.vertexPolygons.size()
                  << ", Vertices: " << result.totalVertexCount() << std::endl;
        if (result.totalVertexCount() == 0) {
            std::cerr << "  FAIL: Pyramid produced 0 vertices" << std::endl;
            allPassed = false;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    {
        std::cout << "\n=== Cylinder Tessellation ===" << std::endl;
        OmegaGTE::GCylinder cylinder{};
        cylinder.pos = {0, 0, 0};
        cylinder.r = 50;
        cylinder.h = 200;
        auto params = OmegaGTE::TETriangulationParams::Cylinder(cylinder);
        auto result = teCtx->triangulateSync(params, OmegaGTE::GTEPolygonFrontFaceRotation::Clockwise, &vp);
        std::cout << "  Polygons: " << result.mesh.vertexPolygons.size()
                  << ", Vertices: " << result.totalVertexCount() << std::endl;
        if (result.totalVertexCount() == 0) {
            std::cerr << "  FAIL: Cylinder produced 0 vertices" << std::endl;
            allPassed = false;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    {
        std::cout << "\n=== Cone Tessellation ===" << std::endl;
        OmegaGTE::GCone cone{};
        cone.x = 0; cone.y = 0; cone.z = 0;
        cone.r = 50;
        cone.h = 150;
        auto params = OmegaGTE::TETriangulationParams::Cone(cone);
        auto result = teCtx->triangulateSync(params, OmegaGTE::GTEPolygonFrontFaceRotation::Clockwise, &vp);
        std::cout << "  Polygons: " << result.mesh.vertexPolygons.size()
                  << ", Vertices: " << result.totalVertexCount() << std::endl;
        if (result.totalVertexCount() == 0) {
            std::cerr << "  FAIL: Cone produced 0 vertices" << std::endl;
            allPassed = false;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    {
        std::cout << "\n=== GraphicsPath3D Tessellation ===" << std::endl;
        OmegaGTE::GVectorPath3D path3d(OmegaGTE::GPoint3D{0, 0, 0});
        path3d.append(OmegaGTE::GPoint3D{100, 0, 0});
        path3d.append(OmegaGTE::GPoint3D{100, 100, 0});
        path3d.append(OmegaGTE::GPoint3D{0, 100, 0});
        path3d.append(OmegaGTE::GPoint3D{0, 0, 0});
        auto params = OmegaGTE::TETriangulationParams::GraphicsPath3D(1, &path3d);
        auto result = teCtx->triangulateSync(params, OmegaGTE::GTEPolygonFrontFaceRotation::Clockwise, &vp);
        std::cout << "  Polygons: " << result.mesh.vertexPolygons.size()
                  << ", Vertices: " << result.totalVertexCount() << std::endl;
        if (result.totalVertexCount() == 0) {
            std::cerr << "  FAIL: GraphicsPath3D produced 0 vertices" << std::endl;
            allPassed = false;
        } else {
            std::cout << "  PASS" << std::endl;
        }
    }

    if (allPassed) {
        std::cout << "\n=== ALL CPU TESSELLATION TESTS PASSED ===" << std::endl;
        return 0;
    }
    std::cout << "\n=== SOME CPU TESSELLATION TESTS FAILED ===" << std::endl;
    return 1;
}

GTE_TEST_ENTRY_POINT {
    (void)argc;

    gte = OmegaGTE::InitWithDefaultDevice();
    std::cout << "GTE Initialized" << std::endl;

    OmegaGTETests::GTETestWindowDescriptor desc;
    desc.title = "GTE CPUTessTest";
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

        int exitCode = runTests();
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
