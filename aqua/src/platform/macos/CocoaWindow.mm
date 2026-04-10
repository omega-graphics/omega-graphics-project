#include <aqua/Window.h>
#include <omegaGTE/GE.h>

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>

// A minimal NSView subclass backed by a CAMetalLayer.
@interface AquaMetalView : NSView
@end

@implementation AquaMetalView

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        self.wantsLayer = YES;
    }
    return self;
}

- (CALayer *)makeBackingLayer {
    CAMetalLayer *layer = [CAMetalLayer layer];
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = YES;
    return layer;
}

- (BOOL)wantsUpdateLayer {
    return YES;
}

@end

// Delegate that sets the close flag.
@interface AquaWindowDelegate : NSObject <NSWindowDelegate>
@property (nonatomic) BOOL shouldClose;
@end

@implementation AquaWindowDelegate

- (instancetype)init {
    self = [super init];
    if (self) {
        _shouldClose = NO;
    }
    return self;
}

- (BOOL)windowShouldClose:(NSWindow *)sender {
    _shouldClose = YES;
    return YES;
}

@end

namespace Aqua {

struct Window::Impl {
    NSWindow *nsWindow;
    AquaMetalView *metalView;
    AquaWindowDelegate *delegate;
    unsigned w;
    unsigned h;
};

Window::Window() : impl(std::make_unique<Impl>()) {}

Window::~Window() {
    if (impl->nsWindow) {
        [impl->nsWindow close];
    }
}

std::unique_ptr<Window> Window::create(const WindowDesc &desc) {
    // Ensure NSApplication is initialized for event processing.
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    auto window = std::unique_ptr<Window>(new Window());
    window->impl->w = desc.width;
    window->impl->h = desc.height;

    NSRect frame = NSMakeRect(100, 100, desc.width, desc.height);
    NSUInteger styleMask = NSWindowStyleMaskTitled
                         | NSWindowStyleMaskClosable
                         | NSWindowStyleMaskMiniaturizable
                         | NSWindowStyleMaskResizable;

    window->impl->nsWindow = [[NSWindow alloc]
        initWithContentRect:frame
        styleMask:styleMask
        backing:NSBackingStoreBuffered
        defer:NO];

    NSString *title = [NSString stringWithUTF8String:desc.title];
    [window->impl->nsWindow setTitle:title];

    window->impl->delegate = [[AquaWindowDelegate alloc] init];
    [window->impl->nsWindow setDelegate:window->impl->delegate];

    window->impl->metalView = [[AquaMetalView alloc] initWithFrame:frame];
    [window->impl->nsWindow setContentView:window->impl->metalView];

    [window->impl->nsWindow makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];

    return window;
}

bool Window::shouldClose() const {
    return impl->delegate.shouldClose;
}

void Window::pollEvents() {
    @autoreleasepool {
        NSEvent *event;
        while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                           untilDate:nil
                                              inMode:NSDefaultRunLoopMode
                                             dequeue:YES])) {
            [NSApp sendEvent:event];
        }
    }
}

unsigned Window::width() const { return impl->w; }
unsigned Window::height() const { return impl->h; }

void Window::fillNativeRenderTargetDesc(OmegaGTE::NativeRenderTargetDescriptor &desc) const {
    CAMetalLayer *layer = (CAMetalLayer *)[impl->metalView layer];
    CGFloat scale = impl->nsWindow.backingScaleFactor;
    layer.contentsScale = scale;
    layer.drawableSize = CGSizeMake(impl->w * scale, impl->h * scale);
    desc.metalLayer = layer;
}

} // namespace Aqua
