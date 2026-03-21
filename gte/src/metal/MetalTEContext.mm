#include "omegaGTE/TE.h"
#include "GEMetalRenderTarget.h"
#include "GEMetalTexture.h"

#import <simd/simd.h>
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>

#include <cmath>

_NAMESPACE_BEGIN_

namespace {

struct MetalTessVertex { simd_float4 pos; simd_float4 color; };
struct MetalTessParams { simd_float4 rect; simd_float4 viewport; simd_float4 color; simd_float4 extra; };
struct PathSeg { simd_float4 se; simd_float4 sv; simd_float4 c; simd_float4 r; };

NSString *tessRectSrc = @R"(
#include <metal_stdlib>
using namespace metal;
struct P{float4 r;float4 v;float4 c;float4 e;};struct V{float4 p;float4 c;};
kernel void k(device const P*p[[buffer(0)]],device V*o[[buffer(1)]],uint t[[thread_position_in_grid]]){
float x=p[0].r[0],y=p[0].r[1],w=p[0].r[2],h=p[0].r[3],vw=p[0].v[2],vh=p[0].v[3];float4 c=p[0].c;
float a=(2*x)/vw,b=(2*y)/vh,d=(2*(x+w))/vw,e=(2*(y+h))/vh;V z;z.c=c;
z.p=float4(a,b,0,1);o[0]=z;z.p=float4(a,e,0,1);o[1]=z;z.p=float4(d,b,0,1);o[2]=z;
z.p=float4(d,e,0,1);o[3]=z;z.p=float4(a,e,0,1);o[4]=z;z.p=float4(d,b,0,1);o[5]=z;})";

NSString *tessEllipSrc = @R"(
#include <metal_stdlib>
using namespace metal;
struct P{float4 r;float4 v;float4 c;float4 e;};struct V{float4 p;float4 c;};
kernel void k(device const P*p[[buffer(0)]],device V*o[[buffer(1)]],uint t[[thread_position_in_grid]]){
float cx=p[0].r[0],cy=p[0].r[1],rx=p[0].e[0],ry=p[0].e[1],ts=p[0].e[3];
float vw=p[0].v[2],vh=p[0].v[3];float4 c=p[0].c;float pi2=6.28318530718;
float a0=(pi2*float(t))/ts,a1=(pi2*float(t+1))/ts;uint b=t*3;V z;z.c=c;
z.p=float4((2*cx)/vw,(2*cy)/vh,0,1);o[b]=z;
z.p=float4((2*(cx+rx*cos(a0)))/vw,(2*(cy+ry*sin(a0)))/vh,0,1);o[b+1]=z;
z.p=float4((2*(cx+rx*cos(a1)))/vw,(2*(cy+ry*sin(a1)))/vh,0,1);o[b+2]=z;})";

NSString *tessRPrismSrc = @R"(
#include <metal_stdlib>
using namespace metal;
struct P{float4 r;float4 v;float4 c;float4 e;};struct V{float4 p;float4 c;};
kernel void k(device const P*p[[buffer(0)]],device V*o[[buffer(1)]],uint t[[thread_position_in_grid]]){
float x0=p[0].r[0],y0=p[0].r[1],z0=p[0].r[2],w=p[0].r[3],h=p[0].e[0],d=p[0].e[1];
float vw=p[0].v[2],vh=p[0].v[3];float4 c=p[0].c;
float a=(2*x0)/vw,b=(2*y0)/vh,e=(2*(x0+w))/vw,f=(2*(y0+h))/vh,g=z0,j=z0+d;
V z;z.c=c;uint i=0;
z.p=float4(a,b,g,1);o[i++]=z;z.p=float4(a,f,g,1);o[i++]=z;z.p=float4(e,b,g,1);o[i++]=z;
z.p=float4(e,f,g,1);o[i++]=z;z.p=float4(a,f,g,1);o[i++]=z;z.p=float4(e,b,g,1);o[i++]=z;
z.p=float4(a,b,j,1);o[i++]=z;z.p=float4(e,b,j,1);o[i++]=z;z.p=float4(a,f,j,1);o[i++]=z;
z.p=float4(e,f,j,1);o[i++]=z;z.p=float4(e,b,j,1);o[i++]=z;z.p=float4(a,f,j,1);o[i++]=z;
z.p=float4(a,b,g,1);o[i++]=z;z.p=float4(a,b,j,1);o[i++]=z;z.p=float4(a,f,g,1);o[i++]=z;
z.p=float4(a,f,j,1);o[i++]=z;z.p=float4(a,b,j,1);o[i++]=z;z.p=float4(a,f,g,1);o[i++]=z;
z.p=float4(e,b,g,1);o[i++]=z;z.p=float4(e,f,g,1);o[i++]=z;z.p=float4(e,b,j,1);o[i++]=z;
z.p=float4(e,f,j,1);o[i++]=z;z.p=float4(e,f,g,1);o[i++]=z;z.p=float4(e,b,j,1);o[i++]=z;
z.p=float4(a,b,g,1);o[i++]=z;z.p=float4(e,b,g,1);o[i++]=z;z.p=float4(a,b,j,1);o[i++]=z;
z.p=float4(e,b,j,1);o[i++]=z;z.p=float4(e,b,g,1);o[i++]=z;z.p=float4(a,b,j,1);o[i++]=z;
z.p=float4(a,f,g,1);o[i++]=z;z.p=float4(a,f,j,1);o[i++]=z;z.p=float4(e,f,g,1);o[i++]=z;
z.p=float4(e,f,j,1);o[i++]=z;z.p=float4(a,f,j,1);o[i++]=z;z.p=float4(e,f,g,1);o[i++]=z;})";

NSString *tessPathSrc = @R"(
#include <metal_stdlib>
using namespace metal;
struct S{float4 se;float4 sv;float4 c;float4 r;};struct V{float4 p;float4 c;};
kernel void k(device const S*s[[buffer(0)]],device V*v[[buffer(1)]],uint t[[thread_position_in_grid]]){
float sx=s[t].se[0],sy=s[t].se[1],ex=s[t].se[2],ey=s[t].se[3];
float sw=s[t].sv[0],vw=s[t].sv[2],vh=s[t].sv[3];float4 c=s[t].c;
float dx=ex-sx,dy=ey-sy,l=sqrt(dx*dx+dy*dy),hw=sw*0.5;
float nx=0,ny=0;if(l>0.0001){nx=-dy/l*hw;ny=dx/l*hw;}
uint b=t*6;V o;o.c=c;
o.p=float4((2*(sx+nx))/vw,(2*(sy+ny))/vh,0,1);v[b]=o;
o.p=float4((2*(sx-nx))/vw,(2*(sy-ny))/vh,0,1);v[b+1]=o;
o.p=float4((2*(ex+nx))/vw,(2*(ey+ny))/vh,0,1);v[b+2]=o;
o.p=float4((2*(ex+nx))/vw,(2*(ey+ny))/vh,0,1);v[b+3]=o;
o.p=float4((2*(sx-nx))/vw,(2*(sy-ny))/vh,0,1);v[b+4]=o;
o.p=float4((2*(ex-nx))/vw,(2*(ey-ny))/vh,0,1);v[b+5]=o;})";

id<MTLComputePipelineState> compileKernel(id<MTLDevice> dev, NSString *src) {
    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:&err];
    if (!lib) { NSLog(@"MetalTE: compile error: %@", err); return nil; }
    id<MTLFunction> fn = [lib newFunctionWithName:@"k"];
    if (!fn) return nil;
    return [dev newComputePipelineStateWithFunction:fn error:&err];
}

TETessellationResult readback(id<MTLBuffer> buf, unsigned vc,
                              const std::optional<TETessellationResult::AttachmentData> &att) {
    TETessellationResult res;
    TETessellationResult::TEMesh mesh{TETessellationResult::TEMesh::TopologyTriangle};
    auto *v = (MetalTessVertex *)[buf contents];
    for (unsigned i = 0; i + 2 < vc; i += 3) {
        TETessellationResult::TEMesh::Polygon p{};
        p.a.pt = {v[i].pos[0], v[i].pos[1], v[i].pos[2]};
        p.b.pt = {v[i+1].pos[0], v[i+1].pos[1], v[i+1].pos[2]};
        p.c.pt = {v[i+2].pos[0], v[i+2].pos[1], v[i+2].pos[2]};
        if (att) p.a.attachment = p.b.attachment = p.c.attachment = att;
        mesh.vertexPolygons.push_back(p);
    }
    res.meshes.push_back(mesh);
    return res;
}

struct Pipelines {
    id<MTLDevice> dev = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLComputePipelineState> rect = nil, ellip = nil, prism = nil, path = nil;
    bool ready = false;
    void init(id<MTLDevice> d, id<MTLCommandQueue> q) {
        if (ready) return; ready = true; dev = d; queue = q;
        rect = compileKernel(d, tessRectSrc);
        ellip = compileKernel(d, tessEllipSrc);
        prism = compileKernel(d, tessRPrismSrc);
        path = compileKernel(d, tessPathSrc);
    }
};

std::future<TETessellationResult> gpuDispatch(
        OmegaTessellationEngineContext::GPUTessExtractedParams &ep,
        GEViewport &vp, float ctxArcStep, Pipelines &pip,
        OmegaTessellationEngineContext *ctx,
        const TETessellationParams &origParams,
        GTEPolygonFrontFaceRotation ff, GEViewport *origVP) {

    std::optional<TETessellationResult::AttachmentData> colorAtt;
    simd_float4 cv = {0,0,0,1};
    if (ep.hasColor) {
        colorAtt = TETessellationResult::AttachmentData{FVec<4>::Create(), FVec<2>::Create(), FVec<3>::Create()};
        colorAtt->color[0][0] = ep.cr; colorAtt->color[1][0] = ep.cg;
        colorAtt->color[2][0] = ep.cb; colorAtt->color[3][0] = ep.ca;
        cv = {ep.cr, ep.cg, ep.cb, ep.ca};
    }

    auto fallback = [&]() {
        auto r = ctx->tessalateSync(origParams, ff, origVP);
        std::promise<TETessellationResult> p; p.set_value(std::move(r)); return p.get_future();
    };

    id<MTLComputePipelineState> pso = nil;
    unsigned vc = 0;
    NSUInteger tc = 1;
    id<MTLBuffer> paramBuf = nil, outBuf = nil;

    using ET = OmegaTessellationEngineContext::GPUTessExtractedParams;
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

    auto prom = std::make_shared<std::promise<TETessellationResult>>();
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


class MetalNativeRenderTargetTEContext : public OmegaTessellationEngineContext {
    SharedHandle<GEMetalNativeRenderTarget> target;
    Pipelines pip;
public:
    std::future<TETessellationResult> tessalateOnGPU(const TETessellationParams &params,
            GTEPolygonFrontFaceRotation ff, GEViewport *viewport) override {
        if (!pip.ready) {
            id<MTLCommandQueue> q = (id<MTLCommandQueue>)target->nativeCommandQueue();
            pip.init(q.device, q);
        }
        GPUTessExtractedParams ep; extractGPUTessParams(params, ep);
        GEViewport vp = viewport ? *viewport : GEViewport{0,0,1,1,0,1};
        return gpuDispatch(ep, vp, arcStep, pip, this, params, ff, viewport);
    }
    GEViewport getEffectiveViewport() override {
        if(target != nullptr){
            CGFloat s = [NSScreen mainScreen].backingScaleFactor;
            return GEViewport{0.f, 0.f,
                static_cast<float>(target->drawableSize.width / s),
                static_cast<float>(target->drawableSize.height / s),
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

class MetalTextureRenderTargetTEContext : public OmegaTessellationEngineContext {
    SharedHandle<GEMetalTextureRenderTarget> target;
    Pipelines pip;
public:
    std::future<TETessellationResult> tessalateOnGPU(const TETessellationParams &params,
            GTEPolygonFrontFaceRotation ff, GEViewport *viewport) override {
        if (!pip.ready) {
            id<MTLCommandQueue> q = (id<MTLCommandQueue>)target->nativeCommandQueue();
            pip.init(q.device, q);
        }
        GPUTessExtractedParams ep; extractGPUTessParams(params, ep);
        GEViewport vp = viewport ? *viewport : GEViewport{0,0,1,1,0,1};
        return gpuDispatch(ep, vp, arcStep, pip, this, params, ff, viewport);
    }
    GEViewport getEffectiveViewport() override {
        if(target != nullptr){
            CGFloat s = [NSScreen mainScreen].backingScaleFactor;
            id<MTLTexture> tex = (id<MTLTexture>)target->texturePtr->native();
            return GEViewport{0.f, 0.f,
                static_cast<float>(tex.width / s),
                static_cast<float>(tex.height / s),
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
    MetalTextureRenderTargetTEContext(SharedHandle<GEMetalTextureRenderTarget> t) : target(t) {}
};


SharedHandle<OmegaTessellationEngineContext> CreateNativeRenderTargetTEContext(SharedHandle<GENativeRenderTarget> &renderTarget) {
    return (SharedHandle<OmegaTessellationEngineContext>) new MetalNativeRenderTargetTEContext(
        std::dynamic_pointer_cast<GEMetalNativeRenderTarget>(renderTarget));
}

SharedHandle<OmegaTessellationEngineContext> CreateTextureRenderTargetTEContext(SharedHandle<GETextureRenderTarget> &renderTarget) {
    return (SharedHandle<OmegaTessellationEngineContext>) new MetalTextureRenderTargetTEContext(
        std::dynamic_pointer_cast<GEMetalTextureRenderTarget>(renderTarget));
}

_NAMESPACE_END_
