#include <aqua/Window.h>
#include <omegaGTE/GE.h>

#import <UIKit/UIKit.h>
#import <QuartzCore/CAMetalLayer.h>

// AQUA's Window on iOS does not create a UIWindow — UIApplicationMain owns
// the system root window, and `target/ios/main.mm` sets up a UIWindow with
// a CAMetalLayer-backed view as its rootViewController before constructing
// the App. This Window::create call adopts that existing view.
//
// As a result, the WindowDesc fields are largely ignored on iOS:
//   - desc.title:  no title bars on iOS, dropped.
//   - desc.width:  ignored. The system view's bounds (in pixels) are used.
//   - desc.height: ignored. Same.

namespace Aqua {

namespace {

UIWindow *FindActiveUIWindow() {
    if (@available(iOS 13.0, *)) {
        for (UIScene *scene in [UIApplication sharedApplication].connectedScenes) {
            if (![scene isKindOfClass:[UIWindowScene class]]) continue;
            UIWindowScene *windowScene = (UIWindowScene *)scene;
            for (UIWindow *window in windowScene.windows) {
                if (window.isKeyWindow) return window;
            }
            if (windowScene.windows.count > 0) {
                return windowScene.windows.firstObject;
            }
        }
    }
    // Fallback for pre-13 / unscened apps. keyWindow is the only generic
    // pre-scene path; the deprecation warning is silenced because the
    // scene-aware path above is preferred when available.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    return [UIApplication sharedApplication].keyWindow;
#pragma clang diagnostic pop
}

} // namespace

struct Window::Impl {
    // Plain pointers, not __weak: the project compiles ObjC/ObjC++ in MRC
    // (matches the macOS CocoaWindow). The view is retained by the
    // rootViewController, which is retained by the system UIWindow, which
    // lives for the app's lifetime — so these pointers are valid as long
    // as the AQUA Window exists.
    UIView *view = nil;
    CAMetalLayer *metalLayer = nil;
    unsigned w = 0;
    unsigned h = 0;
};

Window::Window() : impl(std::make_unique<Impl>()) {}

Window::~Window() = default;

std::unique_ptr<Window> Window::create(const WindowDesc &desc) {
    (void)desc;

    UIWindow *uiWindow = FindActiveUIWindow();
    if (!uiWindow) {
        // The iOS target's AppDelegate is expected to set up a UIWindow
        // before calling Aqua::CreateApp(). If we get here, the embedder
        // wired the lifecycle in the wrong order.
        return nullptr;
    }

    UIView *rootView = uiWindow.rootViewController.view;
    if (!rootView || ![rootView.layer isKindOfClass:[CAMetalLayer class]]) {
        return nullptr;
    }

    auto window = std::unique_ptr<Window>(new Window());
    window->impl->view = rootView;
    window->impl->metalLayer = (CAMetalLayer *)rootView.layer;

    UIScreen *screen = uiWindow.screen ?: [UIScreen mainScreen];
    CGFloat scale = screen.nativeScale;
    CGSize sizePoints = rootView.bounds.size;
    window->impl->w = (unsigned)(sizePoints.width * scale);
    window->impl->h = (unsigned)(sizePoints.height * scale);

    window->impl->metalLayer.contentsScale = scale;
    window->impl->metalLayer.drawableSize = CGSizeMake(window->impl->w, window->impl->h);
    window->impl->metalLayer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    window->impl->metalLayer.framebufferOnly = YES;

    return window;
}

bool Window::shouldClose() const {
    // iOS apps don't terminate from inside the run loop — the system
    // suspends them. This stays false; lifecycle is owned by the AppDelegate.
    return false;
}

void Window::pollEvents() {
    // The iOS run loop is owned by UIApplicationMain. Touch / lifecycle
    // events flow through the AppDelegate and the responder chain — this
    // call is intentionally empty.
}

unsigned Window::width() const { return impl->w; }
unsigned Window::height() const { return impl->h; }

void Window::fillNativeRenderTargetDesc(OmegaGTE::NativeRenderTargetDescriptor &desc) const {
#if defined(TARGET_METAL) && defined(__OBJC__)
    desc.metalLayer = impl->metalLayer;
#else
    (void)desc;
#endif
}

} // namespace Aqua
