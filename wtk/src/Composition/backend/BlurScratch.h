// Per-layer blur scratch surface for the composition backend.
//
// Phase 2 of Direct-To-Drawable retires the per-context offscreen pair and
// replaces it with on-demand scratch surfaces sized to the layer's bounds.
// `LayerBlurScratch` owns:
//   - `source`     : the texture the layer's primitives render into
//   - `pingPong`   : the compute-pass ping-pong buffer
//   - `sourceTarget` : a GETextureRenderTarget wrapping `source`
//   - `fence`      : signalled by the blur compute pass; awaited by the
//                    composite quad on the swap-chain command buffer
//
// One instance lives per blurred layer for as long as the layer's bounds
// (and pixel format) stay stable. `resize()` is a no-op when nothing
// material changed; otherwise it reallocates from the texture / fence pools
// owned by `BackendResourceFactory`.

#ifndef OMEGAWTK_COMPOSITION_BACKEND_BLURSCRATCH_H
#define OMEGAWTK_COMPOSITION_BACKEND_BLURSCRATCH_H

#include "omegaWTK/Core/GTEHandle.h"

namespace OmegaWTK::Composition {

    class LayerBlurScratch {
        SharedHandle<OmegaGTE::GETexture> source_;
        SharedHandle<OmegaGTE::GETexture> pingPong_;
        SharedHandle<OmegaGTE::GETextureRenderTarget> sourceTarget_;
        SharedHandle<OmegaGTE::GEFence> fence_;
        unsigned width_  = 0;
        unsigned height_ = 0;
        OmegaGTE::PixelFormat pixelFormat_ = OmegaGTE::PixelFormat::BGRA8Unorm;

        void release();
    public:
        LayerBlurScratch();
        ~LayerBlurScratch();

        LayerBlurScratch(const LayerBlurScratch &) = delete;
        LayerBlurScratch & operator=(const LayerBlurScratch &) = delete;

        /// Ensure the scratch is allocated at the requested dimensions and
        /// pixel format. Returns true when a (re)allocation took place.
        /// `width` / `height` are clamped to a minimum of 1 to keep the GPU
        /// allocators happy.
        bool resize(unsigned width, unsigned height,
                    OmegaGTE::PixelFormat pixelFormat);

        /// Returns true once `resize()` has produced valid GPU resources.
        bool valid() const {
            return source_ != nullptr
                && sourceTarget_ != nullptr
                && pingPong_ != nullptr
                && fence_ != nullptr;
        }

        SharedHandle<OmegaGTE::GETexture> & source()                                { return source_; }
        SharedHandle<OmegaGTE::GETexture> & pingPong()                              { return pingPong_; }
        SharedHandle<OmegaGTE::GETextureRenderTarget> & sourceTarget()              { return sourceTarget_; }
        SharedHandle<OmegaGTE::GEFence> & fence()                                   { return fence_; }
        unsigned width()  const { return width_; }
        unsigned height() const { return height_; }
        OmegaGTE::PixelFormat pixelFormat() const { return pixelFormat_; }
    };

}

#endif
