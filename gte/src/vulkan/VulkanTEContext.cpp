

#include "omegaGTE/GTEBase.h"
#include "omegaGTE/TE.h"

#include "GEVulkanRenderTarget.h"
#include <memory>


_NAMESPACE_BEGIN_

class VulkanNativeRenderTargetTEContext : public OmegaTessellationEngineContext {
public:

    SharedHandle<GEVulkanNativeRenderTarget> renderTarget;

    void translateCoords(float x, float y, float z, GEViewport *viewport, float *x_result, float *y_result, float *z_result) override {
        if(viewport != nullptr){
            translateCoordsDefaultImpl(x,y,z,viewport,x_result,y_result,z_result);
        }
        else {
            GEViewport viewport {};
            viewport.x = 0;
            viewport.y = 0;
            viewport.nearDepth = 0.f;
            viewport.farDepth = 1.f;
            viewport.width = (float)renderTarget->extent.width;
            viewport.height = (float)renderTarget->extent.height;
            translateCoordsDefaultImpl(x,y,z,&viewport,x_result,y_result,z_result);
        }
    }

    std::future<TETessellationResult> tessalateOnGPU(const TETessellationParams &params, GEViewport *viewport = nullptr) override {
        return {};
    }

    explicit VulkanNativeRenderTargetTEContext(SharedHandle<GEVulkanNativeRenderTarget> renderTarget):renderTarget(renderTarget){

    };

};


SharedHandle<OmegaTessellationEngineContext> CreateNativeRenderTargetTEContext(SharedHandle<GENativeRenderTarget> & renderTarget){
    return SharedHandle<OmegaTessellationEngineContext>(new VulkanNativeRenderTargetTEContext(std::dynamic_pointer_cast<GEVulkanNativeRenderTarget>(renderTarget)));
};

SharedHandle<OmegaTessellationEngineContext> CreateTextureRenderTargetTEContext(SharedHandle<GETextureRenderTarget> & renderTarget){
    return nullptr;
};

_NAMESPACE_END_
