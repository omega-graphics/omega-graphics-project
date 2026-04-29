#ifndef OMEGAWTK_COMPOSITION_BACKEND_PIPELINE_H
#define OMEGAWTK_COMPOSITION_BACKEND_PIPELINE_H

#include "omegaWTK/Core/GTEHandle.h"
#include <omega-common/utils.h>

namespace OmegaWTK::Composition {

    /// Owns every pipeline state object, the shader library, the fullscreen
    /// quad vertex buffer, and the per-format copy pipeline cache used by the
    /// composition backend. There is exactly one PipelineRegistry, held by
    /// the process-wide BackendResourceFactory.
    class PipelineRegistry {
        SharedHandle<OmegaGTE::GTEShaderLibrary> shaderLibrary_;
        SharedHandle<OmegaGTE::GEBufferWriter> bufferWriter_;
        SharedHandle<OmegaGTE::GERenderPipelineState> color_;
        SharedHandle<OmegaGTE::GERenderPipelineState> texture_;
        SharedHandle<OmegaGTE::GERenderPipelineState> finalCopy_;
        OmegaCommon::Map<OmegaGTE::PixelFormat,
                         SharedHandle<OmegaGTE::GERenderPipelineState>> finalCopyByFormat_;
        SharedHandle<OmegaGTE::GEBuffer> fullscreenQuadBuffer_;

        SharedHandle<OmegaGTE::GEComputePipelineState> linearGradient_;
        SharedHandle<OmegaGTE::GEComputePipelineState> gaussianBlurH_;
        SharedHandle<OmegaGTE::GEComputePipelineState> gaussianBlurV_;
        SharedHandle<OmegaGTE::GEComputePipelineState> directionalBlur_;

        void resetState();
    public:
        /// Compile shader library, build all pipeline state objects and the
        /// fullscreen quad vertex buffer. Safe to call multiple times — each
        /// invocation tears the previous state down first.
        bool initialize();

        /// Release every resource owned by the registry.
        void shutdown();

        SharedHandle<OmegaGTE::GTEShaderLibrary> shaderLibrary() const { return shaderLibrary_; }
        SharedHandle<OmegaGTE::GEBufferWriter> bufferWriter() const { return bufferWriter_; }
        SharedHandle<OmegaGTE::GEBuffer> fullscreenQuadBuffer() const { return fullscreenQuadBuffer_; }

        SharedHandle<OmegaGTE::GERenderPipelineState> color() const { return color_; }
        SharedHandle<OmegaGTE::GERenderPipelineState> texture() const { return texture_; }
        SharedHandle<OmegaGTE::GERenderPipelineState> finalCopy() const { return finalCopy_; }

        /// Lazily construct (and cache) a copy pipeline whose color attachment
        /// matches `fmt`. Returns nullptr if the shader library could not
        /// supply the copy shaders.
        SharedHandle<OmegaGTE::GERenderPipelineState> finalCopyForFormat(OmegaGTE::PixelFormat fmt);

        SharedHandle<OmegaGTE::GEComputePipelineState> linearGradient() const { return linearGradient_; }
        SharedHandle<OmegaGTE::GEComputePipelineState> gaussianBlurH() const { return gaussianBlurH_; }
        SharedHandle<OmegaGTE::GEComputePipelineState> gaussianBlurV() const { return gaussianBlurV_; }
        SharedHandle<OmegaGTE::GEComputePipelineState> directionalBlur() const { return directionalBlur_; }
    };

}

#endif
