#include <OmegaGTE.h>
#include <omegaGTE/GTEShader.h>

#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <AppKit/AppKit.h>
#include <iostream>
#include <cmath>
#include <cassert>

static bool comparePt(OmegaGTE::GPoint3D a, OmegaGTE::GPoint3D b, float tol = 0.01f) {
    return std::fabs(a.x - b.x) < tol &&
           std::fabs(a.y - b.y) < tol &&
           std::fabs(a.z - b.z) < tol;
}

int main(int argc, const char * argv[]) {
    @autoreleasepool {
        auto gte = OmegaGTE::InitWithDefaultDevice();
        std::cout << "GTE Initialized" << std::endl;

        CAMetalLayer *layer = [CAMetalLayer layer];
        layer.device = (__bridge id<MTLDevice>)gte.graphicsEngine->underlyingNativeDevice();
        layer.drawableSize = CGSizeMake(800, 600);
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;

        OmegaGTE::NativeRenderTargetDescriptor rtDesc;
        rtDesc.metalLayer = layer;
        auto renderTarget = gte.graphicsEngine->makeNativeRenderTarget(rtDesc);

        auto teCtx = gte.triangulationEngine->createTEContextFromNativeRenderTarget(renderTarget);
        assert(teCtx && "Failed to create TE context");

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
                  << cpuResult.meshes.size() << " meshes" << std::endl;

        auto gpuFuture = teCtx->triangulateOnGPU(tessParams, OmegaGTE::GTEPolygonFrontFaceRotation::Clockwise, &vp);
        auto gpuResult = gpuFuture.get();
        std::cout << "GPU tessellation: " << gpuResult.totalVertexCount() << " vertices, "
                  << gpuResult.meshes.size() << " meshes" << std::endl;

        bool pass = true;

        if (cpuResult.totalVertexCount() != gpuResult.totalVertexCount()) {
            std::cerr << "FAIL: vertex count mismatch: CPU=" << cpuResult.totalVertexCount()
                      << " GPU=" << gpuResult.totalVertexCount() << std::endl;
            pass = false;
        }

        if (pass && cpuResult.meshes.size() == gpuResult.meshes.size()) {
            for (size_t m = 0; m < cpuResult.meshes.size() && pass; m++) {
                auto &cpuMesh = cpuResult.meshes[m];
                auto &gpuMesh = gpuResult.meshes[m];
                if (cpuMesh.vertexPolygons.size() != gpuMesh.vertexPolygons.size()) {
                    std::cerr << "FAIL: polygon count mismatch in mesh " << m << std::endl;
                    pass = false;
                    break;
                }
                for (size_t p = 0; p < cpuMesh.vertexPolygons.size() && pass; p++) {
                    auto &cp = cpuMesh.vertexPolygons[p];
                    auto &gp = gpuMesh.vertexPolygons[p];
                    if (!comparePt(cp.a.pt, gp.a.pt) ||
                        !comparePt(cp.b.pt, gp.b.pt) ||
                        !comparePt(cp.c.pt, gp.c.pt)) {
                        std::cerr << "FAIL: vertex mismatch in mesh " << m << " polygon " << p << std::endl;
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
        }

        if (pass) {
            std::cout << "PASS: GPU tessellation matches CPU tessellation for rect." << std::endl;
        } else {
            std::cout << "FAIL: GPU tessellation does not match CPU tessellation." << std::endl;
        }

        OmegaGTE::Close(gte);
        return pass ? 0 : 1;
    }
}
