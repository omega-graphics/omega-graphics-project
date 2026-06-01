#include <kreate/App.h>
#include <kreate/Pipeline.h>
#include <kreate/Mesh.h>
#include "renderer/Renderer.h"
#include "pipeline/PipelineFactory.h"
#include "mesh/MeshFactory.h"

namespace Kreate {

struct App::Impl {
    std::unique_ptr<Window> window;
    std::unique_ptr<Renderer> renderer;

    explicit Impl(const AppDesc &desc) {
        window = Window::create(desc.window);
        // The renderer owns the whole OmegaGTE stack (device, queue, render
        // target) and tears it down in its destructor — App stays out of GTE.
        renderer = Renderer::create(*window);
    }
};

App::App(const AppDesc &desc) : impl(std::make_unique<Impl>(desc)) {}

App::~App() = default;

Window &App::window() { return *impl->window; }

std::shared_ptr<Pipeline> App::createPipeline(const std::string &omegaslPath,
                                              const PipelineDesc &desc) {
    return PipelineFactory::create(impl->renderer->gte(), omegaslPath, desc);
}

std::shared_ptr<Pipeline> App::createPipelineFromLibrary(const std::string &libPath,
                                                          const PipelineDesc &desc) {
    return PipelineFactory::createFromLibrary(impl->renderer->gte(), libPath, desc);
}

std::shared_ptr<Mesh> App::createMesh(const MeshDesc &desc,
                                      const void *vertexData,
                                      std::size_t vertexBytes,
                                      unsigned vertexCount,
                                      const void *indexData,
                                      std::size_t indexBytes,
                                      unsigned indexCount) {
    return MeshFactory::create(impl->renderer->gte(), desc,
                               vertexData, vertexBytes, vertexCount,
                               indexData, indexBytes, indexCount);
}

Renderer &App::internalRenderer() { return *impl->renderer; }

void App::run() {
    onInit();
    while (!impl->window->shouldClose()) {
        impl->window->pollEvents();
        onFrame();
    }
}

} // namespace Kreate
