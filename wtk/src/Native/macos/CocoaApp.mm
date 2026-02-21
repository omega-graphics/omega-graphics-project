#import <Cocoa/Cocoa.h>
#include "omegaWTK/Native/NativeApp.h"
#include "omegaWTK/UI/App.h"

#import <Metal/Metal.h>

@interface CocoaThemeObserver : NSObject
-(void)onThemeChange:(NSAppearance *)appearance;
@end


namespace OmegaWTK::Native::Cocoa {

class CocoaApp : public NativeApp {
    void *app;
    CocoaThemeObserver *observer;
public:
    CocoaApp(){
        app = (__bridge void *)[NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        observer = [[CocoaThemeObserver alloc] init];
        [NSApp addObserver:observer forKeyPath:@"effectiveAppearance" options:NSKeyValueObservingOptionNew context:nil];
    };
    void terminate() override{
        [NSApp performSelector:@selector(terminate:) withObject:nil afterDelay:0];
    };
    int runEventLoop() override {
        [NSApp finishLaunching];
        [NSApp activateIgnoringOtherApps:YES];
        [NSApp run];
        return 0;
    };
    ~CocoaApp() override {
        MTLCaptureManager *manager = [MTLCaptureManager sharedCaptureManager];
        [manager stopCapture];
        [NSApp removeObserver:observer forKeyPath:@"effectiveAppearance"];
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

namespace OmegaWTK::Native {
    NAP make_native_app(void *data){
        return (NAP)new Cocoa::CocoaApp();
    }
}
