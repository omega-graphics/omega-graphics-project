#include <aqua/App.h>
#include <aqua/Pipeline.h>
#include "renderer/Renderer.h"
#include "pipeline/PipelineFactory.h"

namespace Aqua {

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

Renderer &App::internalRenderer() { return *impl->renderer; }

void App::run() {
    onInit();
    while (!impl->window->shouldClose()) {
        impl->window->pollEvents();
        onFrame();
    }
}

} // namespace Aqua
