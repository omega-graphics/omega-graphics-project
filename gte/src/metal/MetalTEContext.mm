#include "omegaGTE/TE.h"
#include "GEMetalRenderTarget.h"
#include "GEMetalTexture.h"
// #include "omegaGTE/GTEShaderTypes.h"

#import <simd/simd.h>
#import <AppKit/AppKit.h>

_NAMESPACE_BEGIN_

// OmegaGTEColorVertex *convertVertex(OmegaGTE::GEColoredVertex & vertex){
//     auto * v = new OmegaGTEColorVertex();
//     v->color = simd_float4{vertex.color.valueAt(1,1),vertex.color.valueAt(1,2),vertex.color.valueAt(1,3),vertex.color.valueAt(1,4)};
//     std::cout << "R:" << v->color.r << "G:" << v->color.g << "B:" << v->color.b << "A:" << v->color.a << std::endl;
//     v->pos = {vertex.pos.getI(),vertex.pos.getI(),vertex.pos.getK()};
//     return v;
// };
//
// OmegaGTETexturedVertex *convertVertex(OmegaGTE::GETexturedVertex & vertex){
//     auto * v = new OmegaGTETexturedVertex();
//     v->texturePos = {vertex.textureCoord.getI(),vertex.textureCoord.getJ()};
//     v->pos = {vertex.pos.getI(),vertex.pos.getI(),vertex.pos.getK()};
//     return v;
// };


class MetalNativeRenderTargetTEContext : public OmegaTessellationEngineContext {
    SharedHandle<GEMetalNativeRenderTarget> target;
    public:
    // std::future<TETessalationResult> tessalateAsync(const TETessalationParams &params, std::optional<GEViewport> viewport = {}){
    //     std::promise<TETessalationResult> prom;
    //     auto fut = prom.get_future();
    //     std::thread t([&](std::promise<TETessalationResult> promise){
    //         promise.set_value_at_thread_exit(tessalateSync(params,viewport));
    //     },std::move(prom));
    //     return fut;
    // };
    std::future<TETessellationResult> tessalateOnGPU(const TETessellationParams &params, GTEPolygonFrontFaceRotation frontFaceRotation = GTEPolygonFrontFaceRotation::Clockwise, GEViewport *viewport = nullptr) override {
        return {};
    }
    // TETessalationResult tessalateSync(const TETessalationParams &params, std::optional<GEViewport> viewport = {}){
    //     return _tessalatePriv(params,viewport);
    // };
    void translateCoords(float x, float y,float z,GEViewport * viewport,float *x_result, float *y_result,float *z_result) override{
        // std::cout << viewport << std::endl;
        if(viewport != nullptr){
            translateCoordsDefaultImpl(x,y,z,viewport,x_result,y_result,z_result);
        }
        else {

            std::cout << "Yes" << std::endl;

            CGFloat scaleFactor = [NSScreen mainScreen].backingScaleFactor;

            CGFloat width = target->drawableSize.width / scaleFactor / 2.f;
            CGFloat height = target->drawableSize.height / scaleFactor / 2.f;

            *x_result = x / width;
            *y_result = y / height;

            if(z_result != nullptr) {
                if(z > 0.0){
                    *z_result = z / 1.0;
                }
                else if(z < 0.0){
                    *z_result = z / 1.0;
                }
                else {
                    *z_result = z;
                };
            }

        };
    };
    MetalNativeRenderTargetTEContext(SharedHandle<GEMetalNativeRenderTarget> target):target(target){};
};

class MetalTextureRenderTargetTEContext : public OmegaTessellationEngineContext {
    SharedHandle<GEMetalTextureRenderTarget> target;
    public:
    // std::future<TETessalationResult> tessalateAsync(const TETessalationParams &params, std::optional<GEViewport> viewport = {}){
        
    // };
    std::future<TETessellationResult> tessalateOnGPU(const TETessellationParams &params, GEViewport * viewport){
        {};
    };
    void translateCoords(float x, float y,float z,GEViewport * viewport, float *x_result, float *y_result,float *z_result) override{
        if(viewport != nullptr){
            translateCoordsDefaultImpl(x,y,z,viewport,x_result,y_result,z_result);
        }
        else {
            // std::cout << "Yes" << std::endl;

            CGFloat scaleFactor = [NSScreen mainScreen].backingScaleFactor;

            id<MTLTexture> tex = (id<MTLTexture>)target->texturePtr->native();

            CGFloat width = tex.width / scaleFactor;
            CGFloat height = tex.height / scaleFactor;

            *x_result = x / width;
            *y_result = y / height;

            if(z_result != nullptr) {
                if(z > 0.0){
                    *z_result = z / 1.0;
                }
                else if(z < 0.0){
                    *z_result = z / 1.0;
                }
                else {
                    *z_result = z;
                };
            }
        };
    };
    std::future<TETessellationResult> tessalateOnGPU(const TETessellationParams &params, GTEPolygonFrontFaceRotation frontFaceRotation = GTEPolygonFrontFaceRotation::Clockwise, GEViewport *viewport = nullptr) override {
        return {};
    }
    MetalTextureRenderTargetTEContext(SharedHandle<GEMetalTextureRenderTarget> target):target(target){
        
    };
};

SharedHandle<OmegaTessellationEngineContext> CreateNativeRenderTargetTEContext(SharedHandle<GENativeRenderTarget> &renderTarget){
    return (SharedHandle<OmegaTessellationEngineContext>) new MetalNativeRenderTargetTEContext(std::dynamic_pointer_cast<GEMetalNativeRenderTarget>(renderTarget));
};

SharedHandle<OmegaTessellationEngineContext> CreateTextureRenderTargetTEContext(SharedHandle<GETextureRenderTarget> &renderTarget){
    return (SharedHandle<OmegaTessellationEngineContext>) new MetalTextureRenderTargetTEContext(std::dynamic_pointer_cast<GEMetalTextureRenderTarget>(renderTarget));
};



_NAMESPACE_END_