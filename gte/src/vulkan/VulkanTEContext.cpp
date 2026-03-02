

#include "omegaGTE/GTEBase.h"
#include "omegaGTE/TE.h"

#include "GEVulkanRenderTarget.h"
#include <algorithm>
#include <memory>


_NAMESPACE_BEGIN_

namespace {
    static inline GEViewport makeViewport(float width,float height){
        GEViewport viewport {};
        viewport.x = 0.f;
        viewport.y = 0.f;
        viewport.nearDepth = 0.f;
        viewport.farDepth = 1.f;
        viewport.width = std::max(1.f,width);
        viewport.height = std::max(1.f,height);
        return viewport;
    }
}

class VulkanNativeRenderTargetTEContext : public OmegaTessellationEngineContext {
public:

    SharedHandle<GEVulkanNativeRenderTarget> renderTarget;

    void translateCoords(float x, float y, float z, GEViewport *viewport, float *x_result, float *y_result, float *z_result) override {
        if(viewport != nullptr){
            translateCoordsDefaultImpl(x,y,z,viewport,x_result,y_result,z_result);
        }
        else {
            const float width = renderTarget != nullptr
                                ? static_cast<float>(renderTarget->extent.width)
                                : 1.f;
            const float height = renderTarget != nullptr
                                 ? static_cast<float>(renderTarget->extent.height)
                                 : 1.f;
            auto viewport = makeViewport(width,height);
            translateCoordsDefaultImpl(x,y,z,&viewport,x_result,y_result,z_result);
        }
    }

    std::future<TETessellationResult> tessalateOnGPU(const TETessellationParams &params,GTEPolygonFrontFaceRotation direction, GEViewport *viewport = nullptr) override {
        (void)params;
        (void)direction;
        (void)viewport;
        return {};
    }

    explicit VulkanNativeRenderTargetTEContext(SharedHandle<GEVulkanNativeRenderTarget> renderTarget):renderTarget(renderTarget){

    };

};

class VulkanTextureRenderTargetTEContext : public OmegaTessellationEngineContext {
public:
    SharedHandle<GEVulkanTextureRenderTarget> renderTarget;

    void translateCoords(float x, float y, float z, GEViewport *viewport, float *x_result, float *y_result, float *z_result) override {
        if(viewport != nullptr){
            translateCoordsDefaultImpl(x,y,z,viewport,x_result,y_result,z_result);
        }
        else {
            float width = 1.f;
            float height = 1.f;
            if(renderTarget != nullptr && renderTarget->texture != nullptr){
                width = static_cast<float>(renderTarget->texture->descriptor.width);
                height = static_cast<float>(renderTarget->texture->descriptor.height);
            }
            auto defaultViewport = makeViewport(width,height);
            translateCoordsDefaultImpl(x,y,z,&defaultViewport,x_result,y_result,z_result);
        }
    }

    std::future<TETessellationResult> tessalateOnGPU(const TETessellationParams &params,GTEPolygonFrontFaceRotation direction, GEViewport *viewport = nullptr) override {
        (void)params;
        (void)direction;
        (void)viewport;
        return {};
    }

    explicit VulkanTextureRenderTargetTEContext(SharedHandle<GEVulkanTextureRenderTarget> renderTarget):
    renderTarget(renderTarget){
    };
};


SharedHandle<OmegaTessellationEngineContext> CreateNativeRenderTargetTEContext(SharedHandle<GENativeRenderTarget> & renderTarget){
    auto vulkanRenderTarget = std::dynamic_pointer_cast<GEVulkanNativeRenderTarget>(renderTarget);
    if(vulkanRenderTarget == nullptr){
        return nullptr;
    }
    return SharedHandle<OmegaTessellationEngineContext>(new VulkanNativeRenderTargetTEContext(vulkanRenderTarget));
};

SharedHandle<OmegaTessellationEngineContext> CreateTextureRenderTargetTEContext(SharedHandle<GETextureRenderTarget> & renderTarget){
    auto vulkanRenderTarget = std::dynamic_pointer_cast<GEVulkanTextureRenderTarget>(renderTarget);
    if(vulkanRenderTarget == nullptr){
        return nullptr;
    }
    return SharedHandle<OmegaTessellationEngineContext>(new VulkanTextureRenderTargetTEContext(vulkanRenderTarget));
};

_NAMESPACE_END_
