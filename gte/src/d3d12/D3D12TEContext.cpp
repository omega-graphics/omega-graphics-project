#include "GED3D12RenderTarget.h"
#include "omegaGTE/TE.h"

_NAMESPACE_BEGIN_

class D3D12NativeRenderTargetTEContext : public OmegaTessellationEngineContext {
    SharedHandle<GED3D12NativeRenderTarget> target;
    public:
    void translateCoords(float x, float y, float z,GEViewport *viewport, float *x_result, float *y_result, float *z_result){
        if(viewport != nullptr){
            translateCoordsDefaultImpl(x,y,z,viewport,x_result,y_result,z_result);
        }
        else {
            auto desc = target->renderTargets[target->frameIndex]->GetDesc();
            GEViewport geViewport {0,0,(float)desc.Width,(float)desc.Height};
            translateCoordsDefaultImpl(x,y,z,&geViewport,x_result,y_result,z_result);
        };
    };

    std::future<TETessellationResult> tessalateOnGPU(const TETessellationParams &params,
            GTEPolygonFrontFaceRotation direction, GEViewport *viewport) override {
        GPUTessExtractedParams ep;
        extractGPUTessParams(params, ep);
        auto result = tessalateSync(params, direction, viewport);
        std::promise<TETessellationResult> p;
        p.set_value(std::move(result));
        return p.get_future();
    }

    explicit D3D12NativeRenderTargetTEContext(const SharedHandle<GED3D12NativeRenderTarget> & target):target(target){};
};

class D3D12TextureRenderTargetTEContext : public OmegaTessellationEngineContext {
    SharedHandle<GED3D12TextureRenderTarget> target;
    public:
    void translateCoords(float x, float y, float z, GEViewport *viewport, float *x_result, float *y_result, float *z_result){
        if(viewport != nullptr){
            translateCoordsDefaultImpl(x,y,z,viewport,x_result,y_result,z_result);
        }
        else {
            auto desc = target->texture->resource->GetDesc();
            GEViewport geViewport {0,0,(float)desc.Width,(float)desc.Height};
            translateCoordsDefaultImpl(x,y,z,&geViewport,x_result,y_result,z_result);
        };
    };

    std::future<TETessellationResult> tessalateOnGPU(const TETessellationParams &params,
            GTEPolygonFrontFaceRotation direction, GEViewport *viewport) override {
        GPUTessExtractedParams ep;
        extractGPUTessParams(params, ep);
        auto result = tessalateSync(params, direction, viewport);
        std::promise<TETessellationResult> p;
        p.set_value(std::move(result));
        return p.get_future();
    }

    explicit D3D12TextureRenderTargetTEContext(const SharedHandle<GED3D12TextureRenderTarget> & target):target(target){};
};

SharedHandle<OmegaTessellationEngineContext> CreateNativeRenderTargetTEContext(SharedHandle<GENativeRenderTarget> &renderTarget){
    return std::make_shared<D3D12NativeRenderTargetTEContext>(std::dynamic_pointer_cast<GED3D12NativeRenderTarget>(renderTarget));
};

SharedHandle<OmegaTessellationEngineContext> CreateTextureRenderTargetTEContext(SharedHandle<GETextureRenderTarget> &renderTarget){
    return std::make_shared<D3D12TextureRenderTargetTEContext>(std::dynamic_pointer_cast<GED3D12TextureRenderTarget>(renderTarget));
};

_NAMESPACE_END_
