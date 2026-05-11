#include <aqua/App.h>
#include <aqua/Pipeline.h>
#include "renderer/Renderer.h"

namespace Aqua {

// Forward-declared in Pipeline.cpp.
struct PipelineFactory {
    static std::shared_ptr<Pipeline> create(OmegaGTE::GTE &gte,
                                            const std::string &omegaslPath,
                                            const PipelineDesc &desc);
    static std::shared_ptr<Pipeline> createFromLibrary(OmegaGTE::GTE &gte,
                                                       const std::string &libPath,
                                                       const PipelineDesc &desc);
};

struct App::Impl {
    std::unique_ptr<Window> window;
    OmegaGTE::GTE gte;
    SharedHandle<OmegaGTE::GECommandQueue> commandQueue;
    SharedHandle<OmegaGTE::GENativeRenderTarget> nativeRenderTarget;
    std::unique_ptr<Renderer> renderer;

    explicit Impl(const AppDesc &desc) {
        window = Window::create(desc.window);

        gte = OmegaGTE::InitWithDefaultDevice();

        OmegaGTE::NativeRenderTargetDescriptor rtDesc {};
        window->fillNativeRenderTargetDesc(rtDesc);

        commandQueue = gte.graphicsEngine->makeCommandQueue(64);
        nativeRenderTarget = gte.graphicsEngine->makeNativeRenderTarget(rtDesc, commandQueue);

        renderer = std::make_unique<Renderer>(nativeRenderTarget, commandQueue);
    }

    ~Impl() {
        renderer.reset();
        nativeRenderTarget.reset();
        commandQueue.reset();
        OmegaGTE::Close(gte);
    }
};

App::App(const AppDesc &desc) : impl(std::make_unique<Impl>(desc)) {}

App::~App() = default;

Window &App::window() { return *impl->window; }

std::shared_ptr<Pipeline> App::createPipeline(const std::string &omegaslPath,
                                              const PipelineDesc &desc) {
    return PipelineFactory::create(impl->gte, omegaslPath, desc);
}

std::shared_ptr<Pipeline> App::createPipelineFromLibrary(const std::string &libPath,
                                                          const PipelineDesc &desc) {
    return PipelineFactory::createFromLibrary(impl->gte, libPath, desc);
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
