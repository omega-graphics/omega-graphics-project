#include <aqua/App.h>

namespace Aqua {

struct App::Impl {
    std::unique_ptr<Window> window;
    OmegaGTE::GTE gte;
    SharedHandle<OmegaGTE::GECommandQueue> commandQueue;
    SharedHandle<OmegaGTE::GENativeRenderTarget> nativeRenderTarget;

    explicit Impl(const AppDesc &desc) {
        window = Window::create(desc.window);

        gte = OmegaGTE::InitWithDefaultDevice();

        OmegaGTE::NativeRenderTargetDescriptor rtDesc {};
        window->fillNativeRenderTargetDesc(rtDesc);

        commandQueue = gte.graphicsEngine->makeCommandQueue(64);
        nativeRenderTarget = gte.graphicsEngine->makeNativeRenderTarget(rtDesc, commandQueue);
    }

    ~Impl() {
        nativeRenderTarget.reset();
        commandQueue.reset();
        OmegaGTE::Close(gte);
    }
};

App::App(const AppDesc &desc) : impl(std::make_unique<Impl>(desc)) {}

App::~App() = default;

Window &App::window() { return *impl->window; }

OmegaGTE::GTE &App::gte() { return impl->gte; }

SharedHandle<OmegaGTE::GENativeRenderTarget> &App::renderTarget() {
    return impl->nativeRenderTarget;
}

SharedHandle<OmegaGTE::GECommandQueue> &App::commandQueue() {
    return impl->commandQueue;
}

void App::run() {
    onInit();
    while (!impl->window->shouldClose()) {
        impl->window->pollEvents();
        onFrame();
    }
}

} // namespace Aqua
