#import <Foundation/Foundation.h>

#include "omegaWTK/Native/NativeTimer.h"

#include <memory>

@class OWTKTimerProxy;

namespace OmegaWTK::Native::Cocoa {
class CocoaTimer;
}

@interface OWTKTimerProxy : NSObject
@property (nonatomic, assign) OmegaWTK::Native::Cocoa::CocoaTimer *owner;
- (void)timerDidFire:(NSTimer *)timer;
@end

namespace OmegaWTK::Native::Cocoa {

/// NSTimer-backed timer on the main run loop. The callback fires on
/// the UI thread per `NativeTimer.h`'s contract.
class CocoaTimer : public NativeTimer {
public:
    CocoaTimer(float intervalSec, bool repeats, std::function<void()> callback)
        : interval_(intervalSec > 0.f ? (NSTimeInterval)intervalSec : 0.0),
          repeats_(repeats),
          callback_(std::move(callback)) {
        proxy_ = [[OWTKTimerProxy alloc] init];
        proxy_.owner = this;
        scheduleLocked();
    }

    ~CocoaTimer() override {
        cancelLocked();
        if(proxy_ != nil){
            proxy_.owner = nullptr;
            [proxy_ release];
            proxy_ = nil;
        }
    }

    void start() override {
        cancelLocked();
        scheduleLocked();
    }

    void stop() override {
        cancelLocked();
    }

    bool isRunning() const override {
        return timer_ != nil && [timer_ isValid];
    }

    /// Invoked from the Obj-C proxy on the main run loop. One-shot
    /// timers are auto-invalidated by NSTimer itself — we still null
    /// `timer_` so `isRunning()` reflects the post-fire state.
    void onFire() {
        std::function<void()> cb = callback_;
        if(!repeats_){
            // NSTimer has already invalidated itself for one-shots.
            timer_ = nil;
        }
        if(cb){
            cb();
        }
    }

private:
    void scheduleLocked() {
        if(proxy_ == nil) return;
        timer_ = [NSTimer scheduledTimerWithTimeInterval:interval_
                                                   target:proxy_
                                                 selector:@selector(timerDidFire:)
                                                 userInfo:nil
                                                  repeats:repeats_ ? YES : NO];
        // Add to common modes so the timer keeps firing during modal
        // tracking loops (menu tracking, drag tracking) — matches
        // user expectation for "main thread timer".
        if(timer_ != nil){
            [[NSRunLoop mainRunLoop] addTimer:timer_ forMode:NSRunLoopCommonModes];
        }
    }

    void cancelLocked() {
        if(timer_ != nil){
            [timer_ invalidate];
            timer_ = nil;
        }
    }

    NSTimeInterval interval_;
    bool repeats_;
    std::function<void()> callback_;
    NSTimer *timer_ = nil;
    OWTKTimerProxy *proxy_ = nil;
};

}

@implementation OWTKTimerProxy
- (void)timerDidFire:(NSTimer *)timer {
    (void)timer;
    OmegaWTK::Native::Cocoa::CocoaTimer *owner = self.owner;
    if(owner != nullptr){
        owner->onFire();
    }
}
@end

namespace OmegaWTK::Native {
    NativeTimerPtr make_native_timer(float intervalSec,
                                      bool repeats,
                                      std::function<void()> callback){
        return std::make_shared<Cocoa::CocoaTimer>(intervalSec, repeats, std::move(callback));
    }
}
