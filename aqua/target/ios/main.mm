#include <aqua/App.h>

#import <UIKit/UIKit.h>
#import <QuartzCore/CAMetalLayer.h>

#include <memory>

// iOS host integration for AQUA.
//
// UIApplicationMain owns the run loop on iOS, so the desktop convention of
// `auto app = Aqua::CreateApp(); app->run();` doesn't apply here. Instead:
//
//   1. main() hands control to UIApplicationMain with our AquaAppDelegate.
//   2. The delegate stands up a UIWindow whose rootViewController hosts a
//      CAMetalLayer-backed UIView (AquaMetalView).
//   3. Once the view is loaded, the delegate calls Aqua::CreateApp() — the
//      AQUA Window adopts the existing view in src/platform/ios/UIKitWindow.mm.
//   4. onInit() runs once, then a CADisplayLink synced to the screen
//      refresh rate drives onFrame() on the main thread.
//   5. App lifecycle (foreground/background) pauses and resumes the link.

// MARK: - Metal-backed view

@interface AquaMetalView : UIView
@end

@implementation AquaMetalView
+ (Class)layerClass { return [CAMetalLayer class]; }

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        self.contentScaleFactor = [UIScreen mainScreen].nativeScale;
        self.opaque = YES;
        self.backgroundColor = [UIColor blackColor];
    }
    return self;
}
@end

// MARK: - View controller

@interface AquaMetalViewController : UIViewController
@end

@implementation AquaMetalViewController
- (void)loadView {
    self.view = [[AquaMetalView alloc] initWithFrame:[UIScreen mainScreen].bounds];
}

- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
@end

// MARK: - App delegate

@interface AquaAppDelegate : UIResponder <UIApplicationDelegate> {
    std::unique_ptr<Aqua::App> _app;
}
@property (nonatomic, strong) UIWindow *window;
@property (nonatomic, strong) AquaMetalViewController *viewController;
@property (nonatomic, strong) CADisplayLink *displayLink;
@end

@implementation AquaAppDelegate

- (BOOL)application:(UIApplication *)application
    didFinishLaunchingWithOptions:(NSDictionary<UIApplicationLaunchOptionsKey, id> *)launchOptions {
    (void)application;
    (void)launchOptions;

    self.window = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];
    self.viewController = [[AquaMetalViewController alloc] init];
    self.window.rootViewController = self.viewController;
    [self.window makeKeyAndVisible];

    // Force the view hierarchy to materialize so Window::create sees a
    // CAMetalLayer-backed view when the App is constructed below.
    [self.viewController loadViewIfNeeded];

    _app = Aqua::CreateApp();
    if (!_app) {
        return NO;
    }
    _app->onInit();

    self.displayLink = [CADisplayLink displayLinkWithTarget:self selector:@selector(tick:)];
    [self.displayLink addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
    return YES;
}

- (void)tick:(CADisplayLink *)sender {
    (void)sender;
    if (_app) {
        _app->onFrame();
    }
}

- (void)applicationWillResignActive:(UIApplication *)application {
    (void)application;
    self.displayLink.paused = YES;
}

- (void)applicationDidBecomeActive:(UIApplication *)application {
    (void)application;
    self.displayLink.paused = NO;
}

- (void)applicationWillTerminate:(UIApplication *)application {
    (void)application;
    [self.displayLink invalidate];
    self.displayLink = nil;
    _app.reset();
}

@end

// MARK: - Entry point

int main(int argc, char *argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([AquaAppDelegate class]));
    }
}
