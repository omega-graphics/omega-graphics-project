#ifndef AQUA_PIPELINE_H
#define AQUA_PIPELINE_H

#include "Base.h"
#include <memory>
#include <string>

namespace Aqua {

class App;

enum class CullMode { None, Front, Back };
enum class FillMode { Solid, Wireframe };

struct AQUA_EXPORT PipelineDesc {
    CullMode cullMode = CullMode::Back;
    FillMode fillMode = FillMode::Solid;
    bool enableDepth = false;

    /// Name of the vertex shader entry point in the shader library.
    std::string vertexFunction;
    /// Name of the fragment shader entry point in the shader library.
    std::string fragmentFunction;
};

/// Opaque pipeline handle. Constructed only via
/// `App::createPipeline` / `App::createPipelineFromLibrary`.
class AQUA_EXPORT Pipeline {
public:
    ~Pipeline();

    // Non-copyable, movable-only via shared_ptr.
    Pipeline(const Pipeline &) = delete;
    Pipeline &operator=(const Pipeline &) = delete;

private:
    Pipeline();
    struct Impl;
    std::unique_ptr<Impl> impl;

    friend class App;
    friend struct PipelineFactory;
};

} // namespace Aqua

#endif // AQUA_PIPELINE_H
