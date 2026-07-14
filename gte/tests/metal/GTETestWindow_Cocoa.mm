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
//   onClose  fires from RunGTETestWindow right after [NSApp run] returns —
//            matching where the old Metal test called OmegaGTE::Close(gte).
//
// These files are compiled with manual reference counting (no -fobjc-arc, like
// the rest of the Metal backend), so the app delegate / window controller are
// allocated once and intentionally held for the process lifetime.

namespace {
    const OmegaGTETests::GTETestWindowDescriptor *gDesc = nullptr;
    const OmegaGTETests::GTETestWindowDelegate *gDelegate = nullptr;
    int gExitCode = 0;

    // Ends [NSApp run] with `exitCode` as RunGTETestWindow's return value.
    // Used both by the window's close box (windowWillClose, exitCode 0) and by
    // RequestGTETestWindowClose (test-driven, e.g. GPUTessTest's pass/fail).
    //
    // Deliberately NOT [NSApp terminate:]: terminate's happy path posts
    // NSApplicationWillTerminateNotification and then calls exit() itself
    // inside AppKit — control never returns from -run, so RunGTETestWindow
    // could not hand back a caller-chosen exit code. [NSApp stop:] instead
    // marks the run loop to stop and lets -run return normally once it is
    // noticed, so RunGTETestWindow can call onClose once and return gExitCode
    // like every other backend.
    //
    // -stop:'s well-known gotcha: it only takes effect once the run loop
    // processes another event, and does nothing at all if called before the
    // loop has actually started spinning. Calling it from
    // applicationDidFinishLaunching: (i.e. from onReady, for a test that
    // wants to compute a result and exit immediately) races that check, so we
    // post a harmless "application defined" event right after — Apple's own
    // documented workaround — to force the loop to wake up and notice.
    void RequestClose(int exitCode) {
        gExitCode = exitCode;
        [NSApp stop:nil];
        NSEvent *wake = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                            location:NSMakePoint(0, 0)
                                       modifierFlags:0
                                           timestamp:0
                                        windowNumber:0
                                             context:nil
                                             subtype:0
                                               data1:0
                                               data2:0];
        [NSApp postEvent:wake atStart:YES];
    }
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
    RequestClose(0);
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

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender {
    // AppKit registers its own Apple Event handler for "quit" independent of
    // any menu (Dock "Quit", Cmd+Q, `osascript -e 'tell app ... to quit'`),
    // and its default path calls exit() directly — bypassing RequestClose's
    // stop-based flow and, with it, onClose. Redirect every termination
    // request through the same path windowWillClose uses so onClose always
    // fires and the process always returns gExitCode.
    RequestClose(0);
    return NSTerminateCancel;
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
    gExitCode = 0;

    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        [app setDelegate:[[GTETestWindowAppDelegate alloc] init]];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        [app activateIgnoringOtherApps:YES];
        [app run];
    }

    // -run has returned (via RequestClose's [NSApp stop:] path, not
    // terminate:), so onClose fires here — once, on the GUI thread, before
    // this function hands the exit code back to the test's main.
    if (delegate.onClose)
        delegate.onClose();

    return gExitCode;
}

void RequestGTETestWindowClose(int exitCode) {
    RequestClose(exitCode);
}

} // namespace OmegaGTETests
