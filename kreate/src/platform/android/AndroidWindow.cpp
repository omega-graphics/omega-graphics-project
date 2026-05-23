#include <kreate/Window.h>
#include <omegaGTE/GE.h>

// Compile-only stub for Android. The window does not render and exits the
// run loop on the first iteration. Real ANativeWindow / android_native_app_glue
// integration is a follow-up task; this file exists so KREATE links cleanly
// under the Android NDK toolchain.

namespace Kreate {

struct Window::Impl {
    unsigned w = 0;
    unsigned h = 0;
};

Window::Window() : impl(std::make_unique<Impl>()) {}

Window::~Window() = default;

std::unique_ptr<Window> Window::create(const WindowDesc &desc) {
    auto window = std::unique_ptr<Window>(new Window());
    window->impl->w = desc.width;
    window->impl->h = desc.height;
    return window;
}

bool Window::shouldClose() const { return true; }

void Window::pollEvents() {}

unsigned Window::width() const { return impl->w; }
unsigned Window::height() const { return impl->h; }

void Window::fillNativeRenderTargetDesc(OmegaGTE::NativeRenderTargetDescriptor &desc) const {
#if defined(TARGET_VULKAN) && defined(VULKAN_TARGET_ANDROID)
    desc.window = nullptr;
#else
    (void)desc;
#endif
}

} // namespace Kreate
