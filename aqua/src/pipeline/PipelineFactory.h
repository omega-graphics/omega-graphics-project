#ifndef AQUA_INTERNAL_PIPELINE_FACTORY_H
#define AQUA_INTERNAL_PIPELINE_FACTORY_H

#include <aqua/Pipeline.h>
#include <OmegaGTE.h>
#include <memory>
#include <string>

namespace Aqua {

/// Internal entry points for the pipeline subsystem. `App` routes its public
/// `createPipeline*` calls here, handing in the GTE borrowed from the renderer
/// (`Renderer::gte()`). Kept out of public AQUA headers — building a pipeline
/// needs OmegaGTE types that the public surface deliberately hides.
struct PipelineFactory {
    /// Compiles `omegaslPath` at runtime and builds a render pipeline.
    /// Returns nullptr on compile / shader-resolution failure.
    static std::shared_ptr<Pipeline> create(OmegaGTE::GTE &gte,
                                            const std::string &omegaslPath,
                                            const PipelineDesc &desc);

    /// Loads a pre-compiled `.omegasllib` and builds a render pipeline.
    static std::shared_ptr<Pipeline> createFromLibrary(OmegaGTE::GTE &gte,
                                                       const std::string &libPath,
                                                       const PipelineDesc &desc);

    /// Builds a pipeline from an already-loaded shader library. Shared by both
    /// entry points above; lives on the factory so it keeps `Pipeline`'s
    /// friend access to the private constructor.
    static std::shared_ptr<Pipeline> buildFromLibrary(
        OmegaGTE::GTE &gte,
        SharedHandle<OmegaGTE::GTEShaderLibrary> shaderLib,
        const PipelineDesc &desc);
};

} // namespace Aqua

#endif // AQUA_INTERNAL_PIPELINE_FACTORY_H
