#import <Cocoa/Cocoa.h>
#include "omegaWTK/Native/NativeApp.h"
#include "omegaWTK/Native/NativeTheme.h"
#include "omegaWTK/UI/App.h"

#import <Metal/Metal.h>

@interface CocoaThemeObserver : NSObject
-(void)onThemeChange:(NSAppearance *)appearance;
@end

namespace OmegaWTK::Native::Cocoa { class CocoaApp; }

/// Forwards NSApplicationDelegate callbacks to the installed
/// `NativeAppDelegate`. Lives separately from the existing
/// `effectiveAppearance` KVO observer so theme observation stays
/// independent of the app-lifecycle path.
@interface CocoaAppDelegate : NSObject<NSApplicationDelegate>
@property(nonatomic, assign) OmegaWTK::Native::Cocoa::CocoaApp *owner;
@end

namespace OmegaWTK::Native::Cocoa {

class CocoaApp : public NativeApp {
    void *app;
    CocoaThemeObserver *observer;
    CocoaAppDelegate *appDelegate;
public:
    CocoaApp(void *data){
        if(data != nullptr){
            adoptLaunchArgs(*static_cast<NativeAppLaunchArgs *>(data));
        }
        app = (__bridge void *)[NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        observer = [[CocoaThemeObserver alloc] init];
        [NSApp addObserver:observer forKeyPath:@"effectiveAppearance" options:NSKeyValueObservingOptionNew context:nil];
        appDelegate = [[CocoaAppDelegate alloc] init];
        appDelegate.owner = this;
        [NSApp setDelegate:appDelegate];
    };
    void terminate() override{
        if(delegate_ != nullptr){
            delegate_->onAppWillTerminate();
        }
        [NSApp performSelector:@selector(terminate:) withObject:nil afterDelay:0];
    };
    int runEventLoop() override {
        [NSApp finishLaunching];
        [NSApp activateIgnoringOtherApps:YES];
        [NSApp run];
        return 0;
    };
    /// Accessor for the Obj-C delegate forwarder to invoke into.
    NativeAppDelegate * appLevelDelegate() const { return delegate_; }
    ~CocoaApp() override {
        MTLCaptureManager *manager = [MTLCaptureManager sharedCaptureManager];
        [manager stopCapture];
        [NSApp removeObserver:observer forKeyPath:@"effectiveAppearance"];
        if([NSApp delegate] == appDelegate){
            [NSApp setDelegate:nil];
        }
        [appDelegate release];
        [observer release];
    };
};

};

@implementation CocoaThemeObserver
-(void)onThemeChange:(NSAppearance *)appearance {
    OmegaWTK::Native::ThemeDesc desc;
    // OmegaWTK::AppInst::inst()->onThemeSet(desc);
};

- (void)observeValueForKeyPath:(NSString *)keyPath
                      ofObject:(id)object
                        change:(NSDictionary<NSKeyValueChangeKey,id> *)change
                       context:(void *)context {
    if([keyPath isEqualToString:@"effectiveAppearance"]) {
        [self onThemeChange:NSApp.effectiveAppearance];
    }
    else {
        [super observeValueForKeyPath:keyPath ofObject:object change:change context:context];
    }
}
@end

@implementation CocoaAppDelegate
- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    (void)notification;
    if(self.owner == nullptr) return;
    auto * delegate = self.owner->appLevelDelegate();
    if(delegate != nullptr){
        delegate->onAppReady();
    }
}

- (void)applicationWillTerminate:(NSNotification *)notification {
    (void)notification;
    if(self.owner == nullptr) return;
    auto * delegate = self.owner->appLevelDelegate();
    if(delegate != nullptr){
        delegate->onAppWillTerminate();
    }
}

- (BOOL)application:(NSApplication *)sender openFile:(NSString *)filename {
    (void)sender;
    if(self.owner == nullptr) return NO;
    auto * delegate = self.owner->appLevelDelegate();
    if(delegate == nullptr) return NO;
    std::string filenameStr([filename UTF8String]);
    OmegaCommon::FS::Path path(filenameStr);
    delegate->onOpenFile(path);
    return YES;
}

- (void)application:(NSApplication *)application openURLs:(NSArray<NSURL *> *)urls {
    (void)application;
    if(self.owner == nullptr) return;
    auto * delegate = self.owner->appLevelDelegate();
    if(delegate == nullptr) return;
    for(NSURL *url in urls){
        if([url isFileURL]){
            std::string pathStr([[url path] UTF8String]);
            OmegaCommon::FS::Path path(pathStr);
            delegate->onOpenFile(path);
        } else {
            OmegaCommon::String urlStr([[url absoluteString] UTF8String]);
            delegate->onOpenURL(urlStr);
        }
    }
}
@end

namespace OmegaWTK::Native {
    NAP make_native_app(void *data){
        return (NAP)new Cocoa::CocoaApp(data);
    }
}
