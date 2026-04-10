#include <aqua/Window.h>
#include <omegaGTE/GE.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <cstring>

namespace Aqua {

struct Window::Impl {
    ::Display *display = nullptr;
    ::Window xWindow = 0;
    Atom wmDeleteMessage;
    bool closeRequested = false;
    unsigned w = 0;
    unsigned h = 0;
};

Window::Window() : impl(std::make_unique<Impl>()) {}

Window::~Window() {
    if (impl->display) {
        if (impl->xWindow) {
            XDestroyWindow(impl->display, impl->xWindow);
        }
        XCloseDisplay(impl->display);
    }
}

std::unique_ptr<Window> Window::create(const WindowDesc &desc) {
    auto window = std::unique_ptr<Window>(new Window());
    window->impl->w = desc.width;
    window->impl->h = desc.height;

    window->impl->display = XOpenDisplay(nullptr);

    int screen = DefaultScreen(window->impl->display);

    window->impl->xWindow = XCreateSimpleWindow(
        window->impl->display,
        RootWindow(window->impl->display, screen),
        100, 100, desc.width, desc.height,
        1,
        BlackPixel(window->impl->display, screen),
        BlackPixel(window->impl->display, screen));

    XStoreName(window->impl->display, window->impl->xWindow, desc.title);

    window->impl->wmDeleteMessage =
        XInternAtom(window->impl->display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(window->impl->display, window->impl->xWindow,
                    &window->impl->wmDeleteMessage, 1);

    XSelectInput(window->impl->display, window->impl->xWindow,
                 ExposureMask | StructureNotifyMask | KeyPressMask);

    XMapWindow(window->impl->display, window->impl->xWindow);
    XFlush(window->impl->display);

    return window;
}

bool Window::shouldClose() const {
    return impl->closeRequested;
}

void Window::pollEvents() {
    while (XPending(impl->display)) {
        XEvent event;
        XNextEvent(impl->display, &event);
        if (event.type == ClientMessage &&
            (Atom)event.xclient.data.l[0] == impl->wmDeleteMessage) {
            impl->closeRequested = true;
        }
    }
}

unsigned Window::width() const { return impl->w; }
unsigned Window::height() const { return impl->h; }

void Window::fillNativeRenderTargetDesc(OmegaGTE::NativeRenderTargetDescriptor &desc) const {
#if defined(VULKAN_TARGET_X11)
    desc.x_window = impl->xWindow;
    desc.x_display = impl->display;
#endif
}

} // namespace Aqua
