// Canvas-effect processing for the composition backend.
//
// Declares the `BackendCanvasEffectProcessor` interface invoked by the
// per-layer blur path in `BackendRenderTargetContext::renderBlurredSlice`.
// The unified GPU implementation (`GPUCanvasEffectProcessor`, gaussian +
// directional blur via OmegaSL compute) lives in `Effect.cpp`. Phase 4
// retired the per-context fence on the processor: each call to
// `applyEffects` now signals the caller-supplied fence (the per-layer
// scratch's fence), so the processor itself is stateless and a single
// shared instance — vended by `BackendResourceFactory::effectProcessor()`
// — drives every blur in the process.

#ifndef OMEGAWTK_COMPOSITION_BACKEND_EFFECT_H
#define OMEGAWTK_COMPOSITION_BACKEND_EFFECT_H

#include "omegaWTK/Composition/Canvas.h"
#include "omegaWTK/Core/Core.h"
#include "omegaWTK/Core/GTEHandle.h"

namespace OmegaWTK::Composition {

    /// Canvas-effect processor interface. Stateless — a single shared
    /// instance is held by the process-wide `BackendResourceFactory` and
    /// consulted by every per-layer blur composite. The caller supplies
    /// the per-layer scratch's source/pingPong textures, render target,
    /// effects to apply, and the fence the composite pass should wait on.
    INTERFACE BackendCanvasEffectProcessor {
    public:
      INTERFACE_METHOD void applyEffects(SharedHandle<OmegaGTE::GETexture> & dest,
                                         SharedHandle<OmegaGTE::GETextureRenderTarget> & textureTarget,
                                         SharedHandle<OmegaGTE::GECommandQueue> & queue,
                                         OmegaCommon::Vector<CanvasEffect> & effects,
                                         unsigned texWidth,
                                         unsigned texHeight,
                                         SharedHandle<OmegaGTE::GEFence> & fence) ABSTRACT;
      static SharedHandle<BackendCanvasEffectProcessor> Create();
      virtual ~BackendCanvasEffectProcessor() = default;
    };

}

#endif
