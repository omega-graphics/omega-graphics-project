// Frame render-pass control for the composition backend.
//
// Owns the in-flight frame command buffer, the pipeline-bind tracker that
// suppresses redundant rebinds within a frame, the renderingToNative flag
// that picks direct-to-drawable vs offscreen-then-blit, and the viewport
// override. Every render-pass-shaped operation a context performs — frame
// begin/end, single-shot clear, in-frame draw, the standalone
// "no beginFrame" fallback, mid-frame fence restart, pipeline binding —
// flows through `FrameRenderPass`. This module owns no GPU resources of
// its own; it drives the texture set and native target it borrows from
// the owning context.

#ifndef OMEGAWTK_COMPOSITION_BACKEND_RENDERPASS_H
#define OMEGAWTK_COMPOSITION_BACKEND_RENDERPASS_H

#include "omegaWTK/Core/GTEHandle.h"
#include <cstdint>

namespace OmegaWTK::Composition {

    class BackingTextureSet;

    /// A logical-coordinate viewport offset/size that overrides the default
    /// "fill the backing texture" viewport. Stored on FrameRenderPass and
    /// applied at every render-pass start (frame begin, mid-frame restart for
    /// texture fences, and standalone draws).
    struct ViewportOverride {
        bool  active   = false;
        float offsetX  = 0.f;
        float offsetY  = 0.f;
        float width    = 0.f;
        float height   = 0.f;
    };

    /// Owns the frame-level render pass state for a single
    /// BackendRenderTargetContext: the in-flight command buffer, whether a
    /// frame is open, whether the current frame is going direct-to-drawable
    /// or through the offscreen texture, the pipeline-kind tracker that
    /// suppresses redundant pipeline binds within a frame, and the viewport
    /// override.
    ///
    /// Every render-pass-shaped operation that BackendRenderTargetContext
    /// performs — frame begin/end, single-shot clear, in-frame draw, the
    /// standalone "no beginFrame" fallback, mid-frame fence restart, pipeline
    /// binding — flows through this class. The owner only keeps tessellation,
    /// vertex-buffer authoring, and per-draw transform/opacity state.
    class FrameRenderPass {
    public:
        /// Per-draw scope returned by `beginDraw()`. `standalone` is true when
        /// the call opened a fresh render pass on the offscreen target instead
        /// of using the frame's command buffer (the legacy
        /// "renderToTarget without beginFrame" path).
        struct DrawScope {
            SharedHandle<OmegaGTE::GERenderTarget::CommandBuffer> cb;
            bool standalone = false;
        };

    private:
        enum class PipelineKind : std::uint8_t { None, Color, Texture };

        BackingTextureSet & textures_;
        SharedHandle<OmegaGTE::GENativeRenderTarget> & nativeTarget_;

        SharedHandle<OmegaGTE::GERenderTarget::CommandBuffer> frameCB_;
        bool frameActive_       = false;
        bool renderingToNative_ = false;
        PipelineKind lastPipelineKind_ = PipelineKind::None;
        ViewportOverride viewportOverride_;

        /// Apply the current viewport-override (or the default backing-sized
        /// viewport) and matching scissor to `cb`. Used at every
        /// startRenderPass site in this class.
        void applyViewportAndScissor(SharedHandle<OmegaGTE::GERenderTarget::CommandBuffer> & cb);
        void startStandalonePass(SharedHandle<OmegaGTE::GERenderTarget::CommandBuffer> & cb);
    public:
        FrameRenderPass(BackingTextureSet & textures,
                        SharedHandle<OmegaGTE::GENativeRenderTarget> & nativeTarget);

        /// Single-shot render pass that clears the offscreen target to the
        /// given color and immediately ends + submits. No-op when there is no
        /// offscreen target.
        void clearOnce(float r, float g, float b, float a);

        /// Open a frame-level render pass. When `effectsQueued` is false and a
        /// native target is attached, the frame is recorded directly into the
        /// swap chain drawable (eliminating the fullscreen-quad blit on
        /// present); otherwise the frame is recorded into the offscreen
        /// texture for the effect path.
        void begin(float clearR, float clearG, float clearB, float clearA,
                   bool effectsQueued);

        /// End the frame-level render pass and submit its command buffer to
        /// whichever target was selected in begin(). No-op when no frame is
        /// active.
        void end();

        /// Acquire a command buffer for a single draw. Returns the frame's
        /// command buffer when a frame is active; otherwise opens a standalone
        /// render pass on the offscreen target so the legacy
        /// "renderToTarget without beginFrame" path keeps working. When
        /// `textureFence` is non-null, the appropriate fence wait is
        /// registered: a mid-frame end+notify+restart for the in-frame case,
        /// or a plain notify for the standalone case.
        DrawScope beginDraw(SharedHandle<OmegaGTE::GEFence> & textureFence);

        /// Close a draw scope opened by `beginDraw()`. Ends + submits the
        /// standalone command buffer when one was opened; otherwise no-op
        /// (the frame's command buffer is closed by `end()` instead).
        void endDraw(DrawScope & scope);

        /// Bind the color pipeline on `scope.cb`. Suppresses the rebind when
        /// the same pipeline is already bound on the frame's command buffer
        /// (every standalone scope rebinds because each opens a fresh pass).
        void bindColorPipeline(DrawScope & scope);

        /// Same contract as `bindColorPipeline`, for the texture pipeline.
        void bindTexturePipeline(DrawScope & scope);

        void setViewportOverride(float offsetX, float offsetY,
                                 float width, float height);
        void clearViewportOverride();
        const ViewportOverride & viewportOverride() const { return viewportOverride_; }

        bool active()             const { return frameActive_; }
        bool renderingToNative()  const { return renderingToNative_; }

        /// Reset the renderingToNative flag after the owner has presented the
        /// direct-to-drawable frame. Mirrors the legacy
        /// `renderingToNative_ = false` consume in `commit()`.
        void clearRenderingToNative() { renderingToNative_ = false; }
    };

}

#endif
