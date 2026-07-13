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

/// Triangulate `params` on both paths against the same space and assert the two
/// agree vertex-for-vertex. `viewportArg` is the loose call argument; a viewport
/// declared on `params` outranks it (Triangulation-Engine-Completion-Plan Phase
/// 9.2), which is itself one of the things under test here.
static bool compareCase(const char *name,
                        OmegaGTE::TETriangulationParams params,
                        OmegaGTE::GEViewport *viewportArg) {
    const auto winding = OmegaGTE::GTEPolygonFrontFaceRotation::Clockwise;

    auto cpuResult = teCtx->triangulateSync(params, winding, viewportArg);
    auto gpuResult = teCtx->triangulateOnGPU(params, winding, viewportArg).get();

    std::cout << "[" << name << "] CPU: " << cpuResult.totalVertexCount() << " vertices, "
              << cpuResult.mesh.vertexPolygons.size() << " polygons | GPU: "
              << gpuResult.totalVertexCount() << " vertices, "
              << gpuResult.mesh.vertexPolygons.size() << " polygons" << std::endl;

    if (cpuResult.totalVertexCount() != gpuResult.totalVertexCount()) {
        std::cerr << "FAIL [" << name << "]: vertex count mismatch: CPU="
                  << cpuResult.totalVertexCount() << " GPU=" << gpuResult.totalVertexCount()
                  << std::endl;
        return false;
    }

    auto &cpuMesh = cpuResult.mesh;
    auto &gpuMesh = gpuResult.mesh;
    if (cpuMesh.vertexPolygons.size() != gpuMesh.vertexPolygons.size()) {
        std::cerr << "FAIL [" << name << "]: polygon count mismatch" << std::endl;
        return false;
    }

    for (size_t p = 0; p < cpuMesh.vertexPolygons.size(); p++) {
        auto &cp = cpuMesh.vertexPolygons[p];
        auto &gp = gpuMesh.vertexPolygons[p];
        if (comparePt(cp.a.pt, gp.a.pt) &&
            comparePt(cp.b.pt, gp.b.pt) &&
            comparePt(cp.c.pt, gp.c.pt)) {
            continue;
        }
        std::cerr << "FAIL [" << name << "]: vertex mismatch in polygon " << p << std::endl;
        std::cerr << "  CPU: a=(" << cp.a.pt.x << "," << cp.a.pt.y << "," << cp.a.pt.z << ") "
                  << "b=(" << cp.b.pt.x << "," << cp.b.pt.y << "," << cp.b.pt.z << ") "
                  << "c=(" << cp.c.pt.x << "," << cp.c.pt.y << "," << cp.c.pt.z << ")" << std::endl;
        std::cerr << "  GPU: a=(" << gp.a.pt.x << "," << gp.a.pt.y << "," << gp.a.pt.z << ") "
                  << "b=(" << gp.b.pt.x << "," << gp.b.pt.y << "," << gp.b.pt.z << ") "
                  << "c=(" << gp.c.pt.x << "," << gp.c.pt.y << "," << gp.c.pt.z << ")" << std::endl;
        return false;
    }

    std::cout << "  PASS [" << name << "]" << std::endl;
    return true;
}

/// The first vertex of the first polygon — enough to tell two coordinate spaces
/// apart without duplicating the full comparison.
static OmegaGTE::GPoint3D firstVertex(const OmegaGTE::TETriangulationResult &r) {
    if (r.mesh.vertexPolygons.empty()) return OmegaGTE::GPoint3D{0, 0, 0};
    return r.mesh.vertexPolygons.front().a.pt;
}

static int runComparison() {
    OmegaGTE::GEViewport vp{0, 0, 800, 600, 0, 1};

    auto colorVec = OmegaGTE::FVec<4>::Create();
    colorVec[0][0] = 1.0f;
    colorVec[1][0] = 0.0f;
    colorVec[2][0] = 0.0f;
    colorVec[3][0] = 1.0f;
    const auto colorAttachment = OmegaGTE::TETriangulationParams::Attachment::makeColor(colorVec);

    OmegaGTE::GRect rect{OmegaGTE::GPoint2D{100, 100}, 200, 150};

    bool pass = true;

    // --- Baseline: the anchored viewport passed as a call argument. ----------
    {
        auto params = OmegaGTE::TETriangulationParams::Rect(rect);
        params.addAttachment(colorAttachment);
        pass &= compareCase("anchored viewport (call arg)", params, &vp);
    }

    // --- Phase 9.1/9.2: the same space declared on the params instead. Output
    //     must be identical to the call-arg case above, and must still match
    //     GPU-side — this proves the params path and the legacy path agree, and
    //     that the GPU dispatch honors the params viewport too. ---------------
    {
        auto params = OmegaGTE::TETriangulationParams::Rect(rect);
        params.addAttachment(colorAttachment);
        params.viewport = vp;
        pass &= compareCase("anchored viewport (params)", params, nullptr);

        // params.viewport must WIN over a contradicting call argument.
        OmegaGTE::GEViewport decoy{0, 0, 123, 456, 0, 1};
        auto contested = OmegaGTE::TETriangulationParams::Rect(rect);
        contested.addAttachment(colorAttachment);
        contested.viewport = vp;
        pass &= compareCase("params viewport beats call arg", contested, &decoy);

        auto viaParams = teCtx->triangulateSync(
            contested, OmegaGTE::GTEPolygonFrontFaceRotation::Clockwise, &decoy);
        auto viaArg = teCtx->triangulateSync(
            params, OmegaGTE::GTEPolygonFrontFaceRotation::Clockwise, nullptr);
        if (!comparePt(firstVertex(viaParams), firstVertex(viaArg))) {
            std::cerr << "FAIL: the decoy call-arg viewport leaked into the result — "
                         "params.viewport must take precedence" << std::endl;
            pass = false;
        }
    }

    // --- Phase 9.3/9.4: an OFFSET viewport. Its origin must be subtracted, on
    //     both paths. A rect at (100,100) inside a viewport anchored at
    //     (100,100) sits at that viewport's own top-left, so it must map to
    //     NDC (-1,+1) — the exact case the origin-blind code got wrong. -------
    {
        OmegaGTE::GEViewport offset{100, 100, 400, 300, 0, 1};
        auto params = OmegaGTE::TETriangulationParams::Rect(rect);
        params.addAttachment(colorAttachment);
        params.viewport = offset;
        pass &= compareCase("offset viewport", params, nullptr);

        auto res = teCtx->triangulateSync(params, OmegaGTE::GTEPolygonFrontFaceRotation::Clockwise, nullptr);
        const auto v = firstVertex(res);
        if (!comparePt(v, OmegaGTE::GPoint3D{-1.f, 1.f, 0.f})) {
            std::cerr << "FAIL: offset viewport did not honor its origin — the rect's "
                         "corner should land at NDC (-1,+1), got ("
                      << v.x << "," << v.y << "," << v.z << ")" << std::endl;
            pass = false;
        } else {
            std::cout << "  PASS [offset viewport honors origin]" << std::endl;
        }
    }

    // --- Phase 9.6: local space. Both paths must skip the NDC bake entirely
    //     and emit the rect's authored pixel units verbatim. ------------------
    {
        auto params = OmegaGTE::TETriangulationParams::Rect(rect);
        params.addAttachment(colorAttachment);
        params.localSpace = true;
        pass &= compareCase("local space (un-baked)", params, &vp);

        auto res = teCtx->triangulateSync(params, OmegaGTE::GTEPolygonFrontFaceRotation::Clockwise, &vp);
        const auto v = firstVertex(res);
        if (!comparePt(v, OmegaGTE::GPoint3D{100.f, 100.f, 0.f})) {
            std::cerr << "FAIL: local space should emit authored units — expected the rect's "
                         "corner at (100,100,0), got ("
                      << v.x << "," << v.y << "," << v.z << ")" << std::endl;
            pass = false;
        } else {
            std::cout << "  PASS [local space emits authored units]" << std::endl;
        }
    }

    // --- Params-level winding. The kernels bake a {NDC, Clockwise} emission
    //     order, so CounterClockwise used to come out of the GPU with the wrong
    //     winding while the CPU dutifully swapped b/c. Both paths must now agree.
    {
        auto params = OmegaGTE::TETriangulationParams::Rect(rect);
        params.addAttachment(colorAttachment);
        params.viewport = vp;
        params.frontFaceRotation = OmegaGTE::GTEPolygonFrontFaceRotation::CounterClockwise;
        pass &= compareCase("CCW winding (params)", params, nullptr);

        // The CCW result must actually differ from the CW one — otherwise the
        // case above would pass trivially by both paths ignoring the request.
        auto cw = OmegaGTE::TETriangulationParams::Rect(rect);
        cw.addAttachment(colorAttachment);
        cw.viewport = vp;
        auto cwRes = teCtx->triangulateSync(cw, OmegaGTE::GTEPolygonFrontFaceRotation::Clockwise, nullptr);
        auto ccwRes = teCtx->triangulateSync(params, OmegaGTE::GTEPolygonFrontFaceRotation::Clockwise, nullptr);
        auto &cwP = cwRes.mesh.vertexPolygons.front();
        auto &ccwP = ccwRes.mesh.vertexPolygons.front();
        if (!comparePt(cwP.b.pt, ccwP.c.pt) || !comparePt(cwP.c.pt, ccwP.b.pt)) {
            std::cerr << "FAIL: params.frontFaceRotation=CCW did not reverse the winding "
                         "(b/c should be swapped relative to CW)" << std::endl;
            pass = false;
        } else {
            std::cout << "  PASS [params.frontFaceRotation reverses winding, and beats the call arg]"
                      << std::endl;
        }

        // Local space + CCW: both deviations at once. The host XORs them, so this
        // must land back on the SAME winding as NDC+CW — the case most likely to
        // expose a sign error in that XOR.
        auto both = OmegaGTE::TETriangulationParams::Rect(rect);
        both.addAttachment(colorAttachment);
        both.localSpace = true;
        both.frontFaceRotation = OmegaGTE::GTEPolygonFrontFaceRotation::CounterClockwise;
        pass &= compareCase("local space + CCW", both, nullptr);
    }

    if (pass) {
        std::cout << "PASS: GPU tessellation matches CPU tessellation for every "
                     "coordinate-space case." << std::endl;
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
