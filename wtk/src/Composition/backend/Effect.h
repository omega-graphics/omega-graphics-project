// Canvas-effect processing for the composition backend.
//
// Declares the `BackendCanvasEffectProcessor` interface that
// `BackendRenderTargetContext::commit()` invokes between the offscreen draw
// pass and the present blit. The unified GPU implementation
// (`GPUCanvasEffectProcessor`, gaussian + directional blur via OmegaSL
// compute) lives in `Effect.cpp`. Per-context instances are vended through
// `BackendResourceFactory::createEffectProcessor` so the construction site
// is centralized when alternative processors (e.g. CPU fallback) are added.

#ifndef OMEGAWTK_COMPOSITION_BACKEND_EFFECT_H
#define OMEGAWTK_COMPOSITION_BACKEND_EFFECT_H

#include "omegaWTK/Composition/Canvas.h"
#include "omegaWTK/Core/Core.h"
#include "omegaWTK/Core/GTEHandle.h"

namespace OmegaWTK::Composition {

    /// Canvas-effect processor interface. One instance is created per
    /// BackendRenderTargetContext (via `BackendResourceFactory::createEffectProcessor`)
    /// and is invoked from `commit()` after the offscreen draw pass has been
    /// submitted but before the result is blitted to the native drawable.
    ///
    /// The processor is given a destination texture (the ping-pong texture
    /// from the texture set), the offscreen render target whose underlying
    /// texture holds the in-progress draw result, and the queue of effects
    /// to apply. It records the per-effect compute work onto the render
    /// target's command buffer and signals `fence` once the composite pass is
    /// ready for the present blit to consume.
    INTERFACE BackendCanvasEffectProcessor {
    public:
        explicit BackendCanvasEffectProcessor(SharedHandle<OmegaGTE::GEFence> & fence):fence(fence){

        };
        SharedHandle<OmegaGTE::GEFence> fence;
      INTERFACE_METHOD void applyEffects(SharedHandle<OmegaGTE::GETexture> & dest,
                                         SharedHandle<OmegaGTE::GETextureRenderTarget> & textureTarget,
                                         OmegaCommon::Vector<CanvasEffect> & effects,
                                         unsigned texWidth,
                                         unsigned texHeight) ABSTRACT;
      static SharedHandle<BackendCanvasEffectProcessor> Create(SharedHandle<OmegaGTE::GEFence> & fence);
      virtual ~BackendCanvasEffectProcessor() = default;
    };

}

#endif
