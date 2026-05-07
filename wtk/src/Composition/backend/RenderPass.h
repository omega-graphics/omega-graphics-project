// Frame render-pass control for the composition backend.
//
// Owns the in-flight frame command buffer, the pipeline-bind tracker that
// suppresses redundant rebinds within a frame, the viewport override, and
// the per-layer scratch redirect machinery used by Phase 2 blur. Every
// render-pass-shaped operation a context performs — frame begin/end,
// in-frame draw, mid-frame fence restart, pipeline binding — flows through
// `FrameRenderPass` and is recorded directly onto the native swap-chain
// target. This module owns no GPU resources of its own; it borrows from
// the owning `BackendRenderTargetContext` (sizing, native target).

#ifndef OMEGAWTK_COMPOSITION_BACKEND_RENDERPASS_H
#define OMEGAWTK_COMPOSITION_BACKEND_RENDERPASS_H

#include "omegaWTK/Core/GTEHandle.h"
#include <cstdint>

namespace OmegaWTK::Composition {

    class BackendRenderTargetContext;

    /// A logical-coordinate viewport offset/size that overrides the default
    /// "fill the backing texture" viewport. Stored on FrameRenderPass and
    /// applied at every render-pass start (frame begin and mid-frame restart
    /// for texture fences).
    struct ViewportOverride {
        bool  active   = false;
        float offsetX  = 0.f;
        float offsetY  = 0.f;
        float width    = 0.f;
        float height   = 0.f;
    };

    /// Owns the frame-level render pass state for a single
    /// BackendRenderTargetContext: the in-flight command buffer, whether a
    /// frame is open, the pipeline-kind tracker that suppresses redundant
    /// pipeline binds within a frame, and the viewport override.
    ///
    /// Every render-pass-shaped operation that BackendRenderTargetContext
    /// performs — frame begin/end, in-frame draw, mid-frame fence restart,
    /// pipeline binding — flows through this class and is recorded directly
    /// onto the native swap-chain target. The owner only keeps tessellation,
    /// vertex-buffer authoring, and per-draw transform/opacity state.
    class FrameRenderPass {
    public:
        /// Per-draw scope returned by `beginDraw()`. Phase 4 retired the
        /// standalone-CB fallback (every draw now goes through the in-frame
        /// CB or the per-layer scratch CB), so the scope is just a
        /// command-buffer handle for the caller's draw recording.
        struct DrawScope {
            SharedHandle<OmegaGTE::GERenderTarget::CommandBuffer> cb;
        };

    private:
        enum class PipelineKind : std::uint8_t { None, Color, Texture, Sdf, Path, Bitmap };

        BackendRenderTargetContext & owner_;

        SharedHandle<OmegaGTE::GERenderTarget::CommandBuffer> frameCB_;
        bool frameActive_       = false;
        PipelineKind lastPipelineKind_ = PipelineKind::None;
        ViewportOverride viewportOverride_;

        /// Scratch redirection (Phase 2 per-layer blur). When `scratchActive_`
        /// is true, the frame's render pass on the native target has been
        /// suspended (its render pass ended and its CB submitted). Subsequent
        /// `beginDraw()` calls record into `scratchCB_` and use the scratch
        /// target's dimensions for the GPU viewport / scissor. The frame's
        /// render pass is resumed in `endScratchPass()` with `LoadPreserve`
        /// so primitives drawn before the scratch interlude survive.
        SharedHandle<OmegaGTE::GERenderTarget> scratchTarget_;
        SharedHandle<OmegaGTE::GERenderTarget::CommandBuffer> scratchCB_;
        unsigned scratchWidth_  = 0;
        unsigned scratchHeight_ = 0;
        bool scratchActive_ = false;

        /// Apply the current viewport-override (or the default backing-sized
        /// viewport) and matching scissor to `cb`. Used at every
        /// startRenderPass site in this class.
        void applyViewportAndScissor(SharedHandle<OmegaGTE::GERenderTarget::CommandBuffer> & cb);
        void applyScratchViewportAndScissor(SharedHandle<OmegaGTE::GERenderTarget::CommandBuffer> & cb);
    public:
        explicit FrameRenderPass(BackendRenderTargetContext & owner);

        /// Open a frame-level render pass on the native swap-chain target.
        /// Always direct-to-drawable: there is no offscreen intermediate.
        void begin(float clearR, float clearG, float clearB, float clearA);

        /// End the frame-level render pass and submit its command buffer to
        /// the native target. No-op when no frame is active.
        void end();

        /// Acquire the active command buffer for a single draw. Returns the
        /// frame's command buffer when a frame is open and no scratch pass is
        /// in flight; otherwise returns the scratch pass's command buffer.
        /// When `textureFence` is non-null, registers the appropriate fence
        /// wait — for in-frame, this is a mid-pass end+notify+restart so the
        /// wait is recorded outside the active render pass. The returned
        /// scope is closed by `endDraw()` (currently a no-op; the frame's CB
        /// is closed by `end()` and the scratch's by `endScratchPass()`).
        DrawScope beginDraw(SharedHandle<OmegaGTE::GEFence> & textureFence);

        /// Close a draw scope opened by `beginDraw()`. No-op today (the
        /// frame's command buffer is closed by `end()` and the scratch's by
        /// `endScratchPass()`); retained as a stable extension point.
        void endDraw(DrawScope & scope);

        /// Bind the color pipeline on `scope.cb`. Suppresses the rebind when
        /// the same pipeline is already bound in the current pass.
        void bindColorPipeline(DrawScope & scope);

        /// Same contract as `bindColorPipeline`, for the texture pipeline.
        void bindTexturePipeline(DrawScope & scope);

        /// Same contract as `bindColorPipeline`, for the SDF pipeline
        /// (Phase 6). Drives Rect / RoundedRect / Ellipse / Shadow
        /// primitives via the closed-form distance fragment shader.
        void bindSdfPipeline(DrawScope & scope);

        /// Same contract as `bindColorPipeline`, for the path pipeline
        /// (Phase 6.4). Drives `VisualCommand::VectorPath` draws — vertex
        /// layout `(float4 pos, float4 color, float4 edgeTag)` carrying a
        /// per-vertex signed edge distance and attachment tag for AA via
        /// `fwidth(edgeDist)` in the fragment shader.
        void bindPathPipeline(DrawScope & scope);

        /// Same contract as `bindColorPipeline`, for the bitmap pipeline
        /// (Phase 6.6). Drives `VisualCommand::Bitmap` draws — hardcoded
        /// 6-vertex quad authored CPU-side (no triangulator), optional
        /// sub-rect UV baked at vertex authoring time, optional RGBA tint
        /// via a per-draw uniform buffer at fragment slot 10.
        void bindBitmapPipeline(DrawScope & scope);

        void setViewportOverride(float offsetX, float offsetY,
                                 float width, float height);
        void clearViewportOverride();
        const ViewportOverride & viewportOverride() const { return viewportOverride_; }

        bool active() const { return frameActive_; }

        /// Suspend the frame's render pass on the native target and start a
        /// fresh render pass on `scratchTarget` that clears to transparent.
        /// Subsequent `beginDraw()` calls record onto the scratch's command
        /// buffer, with the GPU viewport / scissor sized to (0, 0, w, h).
        ///
        /// Must be called inside an active frame. No-op when no frame is
        /// active or when a scratch pass is already open.
        void beginScratchPass(SharedHandle<OmegaGTE::GETextureRenderTarget> & scratchTarget,
                              unsigned width, unsigned height);

        /// End the scratch render pass and submit its command buffer. The
        /// frame is left in a "suspended" state — `frameActive_` stays true,
        /// but `frameCB_` is null until `resumeFrameAfterScratch()` rebuilds
        /// it. Callers run their compute / effect work between
        /// `endScratchPass()` and `resumeFrameAfterScratch()`.
        void endScratchPass();

        /// Restart the frame's render pass on the native target with
        /// `LoadPreserve` so prior content survives. When `textureFence` is
        /// non-null, register a wait on the new frame CB before starting the
        /// pass — this is how the composite quad waits for the blur compute
        /// to finish writing the scratch.
        void resumeFrameAfterScratch(SharedHandle<OmegaGTE::GEFence> & textureFence);

        bool scratchActive() const { return scratchActive_; }
    };

}

#endif
