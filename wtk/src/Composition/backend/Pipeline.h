// Pipeline state cache for the composition backend.
//
// Owns the shader library, the color and texture render pipeline state
// objects, the blur compute pipelines, and the shared `bufferWriter`.
// Construction and teardown are explicit (`initialize` / `shutdown`) and
// driven by the engine lifecycle thunks in `RenderTarget.cpp`.
//
// Phase 4 retired the `finalCopy` pipeline (and its per-format cache) and
// the standing fullscreen-quad vertex buffer along with the offscreen-then-
// blit path. The per-layer blur composite path authors its own unit-NDC
// quad inline into a buffer-pool buffer.
//
// There is exactly one `PipelineRegistry`, owned by the process-wide
// `BackendResourceFactory`. All consumers obtain pipeline handles through
// that singleton — there are no file-static pipeline globals anywhere in the
// backend.

#ifndef OMEGAWTK_COMPOSITION_BACKEND_PIPELINE_H
#define OMEGAWTK_COMPOSITION_BACKEND_PIPELINE_H

#include "omegaWTK/Core/GTEHandle.h"
#include <omega-common/utils.h>

namespace OmegaWTK::Composition {

    /// Owns every pipeline state object, the shader library, and the shared
    /// buffer writer used by the composition backend. There is exactly one
    /// PipelineRegistry, held by the process-wide BackendResourceFactory.
    class PipelineRegistry {
        SharedHandle<OmegaGTE::GTEShaderLibrary> shaderLibrary_;
        SharedHandle<OmegaGTE::GEBufferWriter> bufferWriter_;
        SharedHandle<OmegaGTE::GERenderPipelineState> color_;
        SharedHandle<OmegaGTE::GERenderPipelineState> texture_;
        /// SDF render pipeline (Phase 6). Drives Rect / RoundedRect /
        /// Ellipse / Shadow primitives via a single closed-form
        /// signed-distance fragment shader. Vertex layout: `(float4
        /// pos, float4 local)`. Per-draw uniform buffer at fragment
        /// stage slot 7 carries shape params, fill / stroke colors,
        /// kind tag, and opacity. Format mirrors the color pipeline:
        /// `BGRA8Unorm` color attachment, no depth, no MSAA.
        SharedHandle<OmegaGTE::GERenderPipelineState> sdf_;
        /// Path render pipeline (Phase 6.4). Drives `VisualCommand::VectorPath`
        /// draws using the GTE triangulator's dual-attachment GraphicsPath2D
        /// output (stroke + fill triangles in one mesh) plus a per-vertex
        /// `(edgeDistance, attachmentTag, _, _)` varying. The fragment shader
        /// derives 1-pixel `smoothstep` AA from `fwidth(edgeDist)`. Vertex
        /// layout: `(float4 pos, float4 color, float4 edgeTag)`.
        SharedHandle<OmegaGTE::GERenderPipelineState> path_;
        /// Bitmap render pipeline (Phase 6.6). Drives `VisualCommand::Bitmap`
        /// draws — hardcoded 6-vertex quad authored CPU-side, optional
        /// sub-rect UV, optional RGBA tint via a per-draw uniform buffer.
        /// Vertex layout: `(float4 pos, float4 uvPad)` (uvPad.xy = sample
        /// UV; .zw reserved). Per-draw uniform at fragment slot 10 carries
        /// the tint color. Texture bound at fragment slot 11. Sampler is
        /// the shared `mainSampler` (anisotropic, clamp_to_edge).
        SharedHandle<OmegaGTE::GERenderPipelineState> bitmap_;

        SharedHandle<OmegaGTE::GEComputePipelineState> linearGradient_;
        SharedHandle<OmegaGTE::GEComputePipelineState> gaussianBlurH_;
        SharedHandle<OmegaGTE::GEComputePipelineState> gaussianBlurV_;
        SharedHandle<OmegaGTE::GEComputePipelineState> directionalBlur_;

        void resetState();
    public:
        /// Compile shader library and build all pipeline state objects.
        /// Safe to call multiple times — each invocation tears the previous
        /// state down first.
        bool initialize();

        /// Release every resource owned by the registry.
        void shutdown();

        SharedHandle<OmegaGTE::GTEShaderLibrary> shaderLibrary() const { return shaderLibrary_; }
        SharedHandle<OmegaGTE::GEBufferWriter> bufferWriter() const { return bufferWriter_; }

        SharedHandle<OmegaGTE::GERenderPipelineState> color() const { return color_; }
        SharedHandle<OmegaGTE::GERenderPipelineState> texture() const { return texture_; }
        SharedHandle<OmegaGTE::GERenderPipelineState> sdf() const { return sdf_; }
        SharedHandle<OmegaGTE::GERenderPipelineState> path() const { return path_; }
        SharedHandle<OmegaGTE::GERenderPipelineState> bitmap() const { return bitmap_; }

        SharedHandle<OmegaGTE::GEComputePipelineState> linearGradient() const { return linearGradient_; }
        SharedHandle<OmegaGTE::GEComputePipelineState> gaussianBlurH() const { return gaussianBlurH_; }
        SharedHandle<OmegaGTE::GEComputePipelineState> gaussianBlurV() const { return gaussianBlurV_; }
        SharedHandle<OmegaGTE::GEComputePipelineState> directionalBlur() const { return directionalBlur_; }
    };

}

#endif
