#include "../GTETestWindow.h"

#include <omegaGTE/GE.h>

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <Metal/Metal.h>

// Cocoa implementation of the cross-backend GTETestWindow surface
// (GTETestWindow-CrossBackend-Plan.md, Phase 2). The NSWindow + NSView +
// CAMetalLayer structure and the NSApplication run loop are lifted from the
// working metal/2DTest/main.mm so the migrated test reaches visual parity. The
// per-test render body and resource teardown stay in the platform-independent
// test source, invoked through the delegate callbacks:
//   onReady  fires from -applicationDidFinishLaunching (window realized, the
//            CAMetalLayer wired into the descriptor),
//   onClose  fires from -applicationWillTerminate, the point the AppKit run
//            loop tears down — matching where the old Metal test called
//            OmegaGTE::Close(gte).
//
// These files are compiled with manual reference counting (no -fobjc-arc, like
// the rest of the Metal backend), so the app delegate / window controller are
// allocated once and intentionally held for the process lifetime.

namespace {
    const OmegaGTETests::GTETestWindowDescriptor *gDesc = nullptr;
    const OmegaGTETests::GTETestWindowDelegate *gDelegate = nullptr;
}

@interface GTETestWindowController : NSWindowController <NSWindowDelegate>
@end

@implementation GTETestWindowController

- (instancetype)init {
    unsigned width  = gDesc ? gDesc->width  : 500;
    unsigned height = gDesc ? gDesc->height : 500;

    NSRect contentRect = NSMakeRect(0, 0, width, height);
    NSWindow *window =
        [[NSWindow alloc] initWithContentRect:contentRect
                                    styleMask:(NSWindowStyleMaskClosable |
                                               NSWindowStyleMaskMiniaturizable |
                                               NSWindowStyleMaskTitled)
                                      backing:NSBackingStoreBuffered
                                        defer:NO];
    if (self = [super initWithWindow:window]) {
        self.window.delegate = self;
        if (gDesc && gDesc->title)
            self.window.title = [NSString stringWithUTF8String:gDesc->title];

        NSView *view = [[NSView alloc] initWithFrame:contentRect];
        view.wantsLayer = YES;

        CAMetalLayer *metalLayer = [CAMetalLayer layer];
        // Render at 1:1 backing so the drawable matches the shared body's
        // logical viewport exactly (the viewport is the descriptor size, not
        // descriptor-size x backingScaleFactor). Retina-crisp rendering would
        // instead track drawableSize in the viewport; left as a follow-up.
        metalLayer.contentsScale = 1.0;
        metalLayer.frame = view.bounds;
        metalLayer.drawableSize = CGSizeMake(width, height);
        view.layer = metalLayer;

        OmegaGTE::NativeRenderTargetDescriptor nrt {};
        nrt.pixelFormat = gDesc ? gDesc->pixelFormat
                                : OmegaGTE::PixelFormat::BGRA8Unorm;
        nrt.allowDepthStencilTesting = gDesc ? gDesc->allowDepthStencilTesting : false;
        nrt.metalLayer = metalLayer;

        // makeNativeRenderTarget (called inside onReady) binds the GTE device
        // onto the layer and reads its drawableSize, so onReady must run after
        // the layer is fully configured above.
        if (gDelegate && gDelegate->onReady)
            gDelegate->onReady(nrt);

        [self.window setContentView:view];
        [self.window center];
    }
    return self;
}

- (void)windowWillClose:(NSNotification *)notification {
    // Terminating drives the run loop down to -applicationWillTerminate:, where
    // the test body's onClose teardown fires before the process exits.
    [NSApp terminate:nil];
}

@end

@interface GTETestWindowAppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation GTETestWindowAppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    // Held for the process lifetime (MRR; never released on purpose).
    GTETestWindowController *windowController =
        [[GTETestWindowController alloc] init];
    [windowController showWindow:self];
    [NSApp activateIgnoringOtherApps:YES];
}

- (void)applicationWillTerminate:(NSNotification *)notification {
    if (gDelegate && gDelegate->onClose)
        gDelegate->onClose();
}

@end

namespace OmegaGTETests {

int RunGTETestWindow(int argc,
                     const char *argv[],
                     const GTETestWindowDescriptor &desc,
                     const GTETestWindowDelegate &delegate) {
    // captureFramePath / argv-driven headless capture is Phase 5; unused here.
    (void)argc;
    (void)argv;

    gDesc = &desc;
    gDelegate = &delegate;

    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        [app setDelegate:[[GTETestWindowAppDelegate alloc] init]];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        [app activateIgnoringOtherApps:YES];
        [app run];
    }

    return 0;
}

} // namespace OmegaGTETests
