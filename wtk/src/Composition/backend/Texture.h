// Per-context backing texture lifecycle for the composition backend.
//
// Owns the offscreen target/effect texture pair, the matching texture render
// targets, the tessellation context that consumes them, and a shared handle
// to the native swap-chain target so the dtor can drain Win32 GPU work and
// `presentBlit()` can copy the effect-path result onto the drawable. Pure
// dimension math (sanitization, backing-dim conversion) lives in this TU
// rather than in the coordinator. This module owns no pipelines and no
// pools — it borrows them from `BackendResourceFactory`.

#ifndef OMEGAWTK_COMPOSITION_BACKEND_TEXTURE_H
#define OMEGAWTK_COMPOSITION_BACKEND_TEXTURE_H

#include "omegaWTK/Composition/Brush.h"
#include "omegaWTK/Composition/Geometry.h"
#include "omegaWTK/Core/GTEHandle.h"

namespace OmegaWTK::Composition {

    /// Owns the offscreen backing texture pair used by a single
    /// BackendRenderTargetContext: a primary `targetTexture` that draw commands
    /// render into, an `effectTexture` used as the ping-pong target during
    /// canvas-effect compute passes, the two TextureRenderTarget wrappers that
    /// drive them, and the tessellation context paired with `preEffectTarget`.
    /// The set also holds a copy of the native (swap chain / drawable) render
    /// target so the dtor can wait for outstanding GPU work before returning
    /// textures to the pool, and `presentBlit()` can copy the effect-path
    /// result onto the swap chain.
    ///
    /// Ownership rule: every texture lives in the per-process `TexturePool`
    /// owned by `BackendResourceFactory`. The set acquires on `rebuild()` and
    /// releases on `rebuild()` / dtor. If the pool is null (legacy fallback)
    /// the textures are allocated directly via the graphics engine.
    class BackingTextureSet {
        SharedHandle<OmegaGTE::GETexture> targetTexture_;
        SharedHandle<OmegaGTE::GETexture> effectTexture_;
        SharedHandle<OmegaGTE::GETextureRenderTarget> preEffectTarget_;
        SharedHandle<OmegaGTE::GETextureRenderTarget> effectTarget_;
        SharedHandle<OmegaGTE::OmegaTriangulationEngineContext> tessellationEngineContext_;
        SharedHandle<OmegaGTE::GENativeRenderTarget> nativeTarget_;
        Composition::Rect renderTargetSize_;
        float renderScale_ = 1.0f;
        unsigned backingWidth_ = 1;
        unsigned backingHeight_ = 1;

        void releaseTexturesToPool();
    public:
        /// Construct a backing set sized to `rect` at the given `renderScale`.
        /// `nativeTarget` may be null for offscreen-only contexts. The
        /// constructor sanitizes the rect/scale and computes the initial
        /// backing dimensions but does not allocate textures — call
        /// `rebuild()` once to perform the initial allocation.
        BackingTextureSet(const Composition::Rect & rect,
                          float renderScale,
                          SharedHandle<OmegaGTE::GENativeRenderTarget> nativeTarget);
        ~BackingTextureSet();

        /// Sanitize the current logical size and recompute backingWidth /
        /// backingHeight from it. Pure dimension math — does not touch the
        /// GPU. Call this before `rebuild()` whenever the logical rect or
        /// render scale has changed.
        void recomputeBackingDimensions();

        /// Release any current textures (returning to the pool when one
        /// exists) and re-acquire textures + render targets + tessellation
        /// context sized to the current backingWidth/backingHeight.
        void rebuild();

        /// Apply a new logical rect. Returns true when the backing dimensions
        /// would change as a result, signalling that the caller should run a
        /// full `rebuild()` (and emit any rebuild traces) at the right point
        /// in its lifecycle.
        bool resizeLogical(const Composition::Rect & rect);

        /// Stamp a viewport-override logical rect (origin reset to 0,0) and
        /// grow the backing surface to cover the requested extent if needed.
        /// Never shrinks. Returns true when the backing must be rebuilt.
        bool applyViewportOverride(float offsetX, float offsetY,
                                   float width, float height);

        /// Blit the effect-path result onto the native render target, fenced.
        /// No-op if there is no native target, no preEffectTarget result, or
        /// no copy pipeline available for the native pixel format.
        void presentBlit(SharedHandle<OmegaGTE::GEFence> & fence);

        /// Compute pass that fills `dest` with a linear or radial gradient
        /// generated from `gradient.stops`. Records into the native target's
        /// command buffer; submits but does not present.
        void uploadGradientTexture(bool linearOrRadial,
                                   Gradient & gradient,
                                   OmegaGTE::GRect & rect,
                                   SharedHandle<OmegaGTE::GETexture> & dest);

        SharedHandle<OmegaGTE::GETexture> & targetTexture()                            { return targetTexture_; }
        SharedHandle<OmegaGTE::GETexture> & effectTexture()                            { return effectTexture_; }
        SharedHandle<OmegaGTE::GETextureRenderTarget> & preEffectTarget()              { return preEffectTarget_; }
        SharedHandle<OmegaGTE::GETextureRenderTarget> & effectTarget()                 { return effectTarget_; }
        SharedHandle<OmegaGTE::OmegaTriangulationEngineContext> & tessellationContext(){ return tessellationEngineContext_; }
        SharedHandle<OmegaGTE::GENativeRenderTarget> & nativeTarget()                  { return nativeTarget_; }

        unsigned backingWidth()  const { return backingWidth_; }
        unsigned backingHeight() const { return backingHeight_; }
        float    renderScale()   const { return renderScale_; }
        const Composition::Rect & renderTargetSize() const { return renderTargetSize_; }
    };

}

#endif
