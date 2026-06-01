#ifndef KREATE_APP_H
#define KREATE_APP_H

#include "Window.h"
#include "Pipeline.h"
#include "Mesh.h"
#include <cstddef>
#include <memory>
#include <string>

namespace Kreate {

class Scene;
class Renderer; // internal — defined in src/renderer/Renderer.h

struct KREATE_EXPORT AppDesc {
    WindowDesc window;
};

class KREATE_EXPORT App {
public:
    explicit App(const AppDesc &desc);
    virtual ~App();

    Window &window();

    /// Compiles `omegaslPath` at runtime and builds a render pipeline.
    /// Returns nullptr on compile / shader-resolution failure.
    std::shared_ptr<Pipeline> createPipeline(const std::string &omegaslPath,
                                              const PipelineDesc &desc);

    /// Loads a pre-compiled `.omegasllib` and builds a render pipeline.
    std::shared_ptr<Pipeline> createPipelineFromLibrary(const std::string &libPath,
                                                         const PipelineDesc &desc);

    /// Allocates GPU buffers for the supplied vertex (and optional index)
    /// data and returns a `Mesh` handle. `vertexData` must be laid out
    /// tightly in the attribute order documented on `VertexAttribute`,
    /// with byte stride equal to `vertexStrideFor(desc.attributes)`. Pass
    /// `indexData == nullptr` (and zero index counts) for non-indexed
    /// meshes; the index variants line up the same way. Returns nullptr
    /// on validation failure with a diagnostic on `std::cerr`.
    std::shared_ptr<Mesh> createMesh(const MeshDesc &desc,
                                     const void *vertexData,
                                     std::size_t vertexBytes,
                                     unsigned vertexCount,
                                     const void *indexData = nullptr,
                                     std::size_t indexBytes = 0,
                                     unsigned indexCount = 0);

    /// Called once after GTE and the render target are ready.
    virtual void onInit() {}

    /// Called every frame. Override this in your game.
    virtual void onFrame() {}

    /// Main loop — called by Main.cpp. Polls events and calls onFrame().
    void run();

private:
    friend class Scene;
    /// Internal accessor for Scene::render to encode through this app's
    /// Renderer. Not part of the public surface.
    Renderer &internalRenderer();

    struct Impl;
    std::unique_ptr<Impl> impl;
};

/// Each game implements this factory. Main.cpp calls it to get the App instance.
KREATE_EXPORT std::unique_ptr<App> CreateApp();

} // namespace Kreate

#endif
