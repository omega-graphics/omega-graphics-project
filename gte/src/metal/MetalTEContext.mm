#include "omegaGTE/TE.h"
#include "GEMetalRenderTarget.h"
#include "GEMetalTexture.h"
#include "../GTEBuiltinShaders.h"

#import <simd/simd.h>
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>

#include <cmath>

_NAMESPACE_BEGIN_

namespace {

struct MetalTessVertex { simd_float4 pos; simd_float4 color; };
struct MetalTessParams { simd_float4 rect; simd_float4 viewport; simd_float4 color; simd_float4 extra; };
struct PathSeg { simd_float4 se; simd_float4 sv; simd_float4 c; simd_float4 r; };

// Triangulation-Engine-Completion-Plan.md Phase 4 -- the four kernels below
// used to be hand-authored Metal C string literals compiled at first use via
// `newLibraryWithSource:`. They are now single-sourced in OmegaSL
// (gte/src/shaders/triangulate_*.omegasl), compiled offline into
// GTEBuiltinShaders.omegasllib, and loaded here from the compiled metallib
// bytes -- same `dispatch_data_create` + `newLibraryWithData:` pattern
// GEMetal.mm's `_loadShaderFromDesc` uses for every other precompiled shader.
id<MTLComputePipelineState> loadBuiltinKernel(id<MTLDevice> dev, const char *name) {
    const omegasl_shader *shader = GTEBuiltinShaders::find(name);
    if (!shader || shader->data == nullptr || shader->dataSize == 0) {
        NSLog(@"MetalTE: builtin kernel '%s' missing from GTEBuiltinShaders", name);
        return nil;
    }
    auto data = dispatch_data_create(shader->data, shader->dataSize, nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithData:data error:&err];
    dispatch_release(data);
    if (!lib) { NSLog(@"MetalTE: newLibraryWithData failed for '%s': %@", name, err); return nil; }
    id<MTLFunction> fn = [lib newFunctionWithName:[NSString stringWithUTF8String:name]];
    if (!fn) { NSLog(@"MetalTE: function '%s' not found in its own library", name); return nil; }
    id<MTLComputePipelineState> pso = [dev newComputePipelineStateWithFunction:fn error:&err];
    if (!pso) { NSLog(@"MetalTE: pipeline creation failed for '%s': %@", name, err); }
    return pso;
}

TETriangulationResult readback(id<MTLBuffer> buf, unsigned vc,
                              const std::optional<TETriangulationResult::AttachmentData> &att) {
    TETriangulationResult res;
    TETriangulationResult::TEMesh mesh{TETriangulationResult::TEMesh::TopologyTriangle};
    auto *v = (MetalTessVertex *)[buf contents];
    for (unsigned i = 0; i + 2 < vc; i += 3) {
        TETriangulationResult::TEMesh::Polygon p{};
        p.a.pt = {v[i].pos[0], v[i].pos[1], v[i].pos[2]};
        p.b.pt = {v[i+1].pos[0], v[i+1].pos[1], v[i+1].pos[2]};
        p.c.pt = {v[i+2].pos[0], v[i+2].pos[1], v[i+2].pos[2]};
        if (att) p.a.attachment = p.b.attachment = p.c.attachment = att;
        mesh.vertexPolygons.push_back(p);
    }
    res.mesh = std::move(mesh);
    return res;
}

struct Pipelines {
    id<MTLDevice> dev = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLComputePipelineState> rect = nil, ellip = nil, prism = nil, path = nil;
    bool ready = false;
    void init(id<MTLDevice> d, id<MTLCommandQueue> q) {
        if (ready) return; ready = true; dev = d; queue = q;
        rect = loadBuiltinKernel(d, GTEBuiltinShaders::TriangulateRect);
        ellip = loadBuiltinKernel(d, GTEBuiltinShaders::TriangulateEllipsoid);
        prism = loadBuiltinKernel(d, GTEBuiltinShaders::TriangulateRectPrism);
        path = loadBuiltinKernel(d, GTEBuiltinShaders::TriangulatePath2D);
    }
};

std::future<TETriangulationResult> gpuDispatch(
        OmegaTriangulationEngineContext::GPUTriangulationExtractedParams &ep,
        GEViewport &vp, float ctxArcStep, Pipelines &pip,
        OmegaTriangulationEngineContext *ctx,
        const TETriangulationParams &origParams,
        GTEPolygonFrontFaceRotation ff, GEViewport *origVP) {

    std::optional<TETriangulationResult::AttachmentData> colorAtt;
    simd_float4 cv = {0,0,0,1};
    if (ep.hasColor) {
        colorAtt = TETriangulationResult::AttachmentData{FVec<4>::Create(), FVec<2>::Create(), FVec<3>::Create()};
        colorAtt->color[0][0] = ep.cr; colorAtt->color[1][0] = ep.cg;
        colorAtt->color[2][0] = ep.cb; colorAtt->color[3][0] = ep.ca;
        cv = {ep.cr, ep.cg, ep.cb, ep.ca};
    }

    auto fallback = [&]() {
        auto r = ctx->triangulateSync(origParams, ff, origVP);
        std::promise<TETriangulationResult> p; p.set_value(std::move(r)); return p.get_future();
    };

    id<MTLComputePipelineState> pso = nil;
    unsigned vc = 0;
    NSUInteger tc = 1;
    id<MTLBuffer> paramBuf = nil, outBuf = nil;

    using ET = OmegaTriangulationEngineContext::GPUTriangulationExtractedParams;
    switch (ep.type) {
        case ET::Rect: {
            pso = pip.rect; vc = 6; tc = 1;
            MetalTessParams tp{{ep.rx,ep.ry,ep.rw,ep.rh},{vp.x,vp.y,vp.width,vp.height},cv,{0,0,0,0}};
            paramBuf = [pip.dev newBufferWithBytes:&tp length:sizeof(tp) options:MTLResourceStorageModeShared];
            outBuf = [pip.dev newBufferWithLength:vc*sizeof(MetalTessVertex) options:MTLResourceStorageModeShared];
            break;
        }
        case ET::Ellipsoid: {
            pso = pip.ellip;
            float step = ctxArcStep > 0 ? ctxArcStep : 0.01f;
            unsigned segs = (unsigned)std::ceil(2.f * M_PI / step);
            vc = segs * 3; tc = segs;
            MetalTessParams tp{{ep.ex,ep.ey,0,0},{vp.x,vp.y,vp.width,vp.height},cv,{ep.erad_x,ep.erad_y,step,(float)segs}};
            paramBuf = [pip.dev newBufferWithBytes:&tp length:sizeof(tp) options:MTLResourceStorageModeShared];
            outBuf = [pip.dev newBufferWithLength:vc*sizeof(MetalTessVertex) options:MTLResourceStorageModeShared];
            break;
        }
        case ET::RectPrism: {
            pso = pip.prism; vc = 36; tc = 1;
            MetalTessParams tp{{ep.px,ep.py,ep.pz,ep.pw},{vp.x,vp.y,vp.width,vp.height},cv,{ep.ph,ep.pd,0,0}};
            paramBuf = [pip.dev newBufferWithBytes:&tp length:sizeof(tp) options:MTLResourceStorageModeShared];
            outBuf = [pip.dev newBufferWithLength:vc*sizeof(MetalTessVertex) options:MTLResourceStorageModeShared];
            break;
        }
        case ET::Path2D: {
            if (ep.pathSegments.empty()) return fallback();
            pso = pip.path;
            unsigned sc = (unsigned)ep.pathSegments.size();
            vc = sc * 6; tc = sc;
            auto *segs = new PathSeg[sc];
            float sw = ep.strokeWidth > 0 ? ep.strokeWidth : 1.f;
            for (unsigned i = 0; i < sc; i++) {
                auto &s = ep.pathSegments[i];
                segs[i] = {{s.sx,s.sy,s.ex,s.ey},{sw,0,vp.width,vp.height},cv,{0,0,0,0}};
            }
            paramBuf = [pip.dev newBufferWithBytes:segs length:sc*sizeof(PathSeg) options:MTLResourceStorageModeShared];
            delete[] segs;
            outBuf = [pip.dev newBufferWithLength:vc*sizeof(MetalTessVertex) options:MTLResourceStorageModeShared];
            break;
        }
        default:
            return fallback();
    }

    if (!pso || !paramBuf || !outBuf) return fallback();

    auto prom = std::make_shared<std::promise<TETriangulationResult>>();
    auto fut = prom->get_future();
    id<MTLCommandBuffer> cmd = [pip.queue commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
    [enc setComputePipelineState:pso];
    [enc setBuffer:paramBuf offset:0 atIndex:0];
    [enc setBuffer:outBuf offset:0 atIndex:1];
    NSUInteger tpg = pso.maxTotalThreadsPerThreadgroup;
    if (tpg > tc) tpg = tc; if (tpg == 0) tpg = 1;
    [enc dispatchThreads:MTLSizeMake(tc,1,1) threadsPerThreadgroup:MTLSizeMake(tpg,1,1)];
    [enc endEncoding];

    auto ca = colorAtt; unsigned vcc = vc;
    [cmd addCompletedHandler:^(id<MTLCommandBuffer>) {
        prom->set_value(readback(outBuf, vcc, ca));
    }];
    [cmd commit];
    return fut;
}

} // anon namespace


class MetalNativeRenderTargetTEContext : public OmegaTriangulationEngineContext {
    SharedHandle<GEMetalNativeRenderTarget> target;
    Pipelines pip;
public:
    std::future<TETriangulationResult> triangulateOnGPU(const TETriangulationParams &params,
            GTEPolygonFrontFaceRotation ff, GEViewport *viewport) override {
        if (!pip.ready && target) {
            auto presentQueue = target->presentQueue();
            if(presentQueue){
                id<MTLCommandQueue> q = (id<MTLCommandQueue>)presentQueue->native();
                pip.init(q.device, q);
            }
        }
        GPUTriangulationExtractedParams ep; extractGPUTriangulationParams(params, ep);
        GEViewport vp = viewport ? *viewport : GEViewport{0,0,1,1,0,1};
        return gpuDispatch(ep, vp, arcStep, pip, this, params, ff, viewport);
    }
    GEViewport getEffectiveViewport() override {
        if(target != nullptr){
            // Phase 7: return raw drawable pixels (matches D3D12 / Vulkan).
            // The legacy `/ backingScaleFactor` divide reported logical
            // points and silently disagreed with the other backends.
            return GEViewport{0.f, 0.f,
                static_cast<float>(target->drawableSize.width),
                static_cast<float>(target->drawableSize.height),
                0.f, 1.f};
        }
        return GEViewport{0.f, 0.f, 1.f, 1.f, 0.f, 1.f};
    }

    void translateCoords(float x, float y, float z, GEViewport *viewport,
                         float *xr, float *yr, float *zr) override {
        if (viewport) { translateCoordsDefaultImpl(x,y,z,viewport,xr,yr,zr); return; }
        auto vp = getEffectiveViewport();
        translateCoordsDefaultImpl(x,y,z,&vp,xr,yr,zr);
    }
    MetalNativeRenderTargetTEContext(SharedHandle<GEMetalNativeRenderTarget> t) : target(t) {}
};

class MetalTextureRenderTargetTEContext : public OmegaTriangulationEngineContext {
    SharedHandle<GEMetalTextureRenderTarget> target;
    SharedHandle<GECommandQueue> queue;
    Pipelines pip;
public:
    std::future<TETriangulationResult> triangulateOnGPU(const TETriangulationParams &params,
            GTEPolygonFrontFaceRotation ff, GEViewport *viewport) override {
        if (!pip.ready && queue) {
            id<MTLCommandQueue> q = (id<MTLCommandQueue>)queue->native();
            pip.init(q.device, q);
        }
        GPUTriangulationExtractedParams ep; extractGPUTriangulationParams(params, ep);
        GEViewport vp = viewport ? *viewport : GEViewport{0,0,1,1,0,1};
        return gpuDispatch(ep, vp, arcStep, pip, this, params, ff, viewport);
    }
    GEViewport getEffectiveViewport() override {
        if(target != nullptr){
            // Phase 7: raw pixels, matching the native-RT path and D3D12 / Vulkan.
            id<MTLTexture> tex = (id<MTLTexture>)target->texturePtr->native();
            return GEViewport{0.f, 0.f,
                static_cast<float>(tex.width),
                static_cast<float>(tex.height),
                0.f, 1.f};
        }
        return GEViewport{0.f, 0.f, 1.f, 1.f, 0.f, 1.f};
    }

    void translateCoords(float x, float y, float z, GEViewport *viewport,
                         float *xr, float *yr, float *zr) override {
        if (viewport) { translateCoordsDefaultImpl(x,y,z,viewport,xr,yr,zr); return; }
        auto vp = getEffectiveViewport();
        translateCoordsDefaultImpl(x,y,z,&vp,xr,yr,zr);
    }
    MetalTextureRenderTargetTEContext(SharedHandle<GEMetalTextureRenderTarget> t,
                                       SharedHandle<GECommandQueue> q) : target(std::move(t)), queue(std::move(q)) {}
};


SharedHandle<OmegaTriangulationEngineContext> CreateNativeRenderTargetTEContext(SharedHandle<GENativeRenderTarget> &renderTarget) {
    return (SharedHandle<OmegaTriangulationEngineContext>) new MetalNativeRenderTargetTEContext(
        std::dynamic_pointer_cast<GEMetalNativeRenderTarget>(renderTarget));
}

SharedHandle<OmegaTriangulationEngineContext> CreateTextureRenderTargetTEContext(SharedHandle<GETextureRenderTarget> &renderTarget,
                                                                                  SharedHandle<GECommandQueue> &queue) {
    return (SharedHandle<OmegaTriangulationEngineContext>) new MetalTextureRenderTargetTEContext(
        std::dynamic_pointer_cast<GEMetalTextureRenderTarget>(renderTarget), queue);
}

_NAMESPACE_END_
