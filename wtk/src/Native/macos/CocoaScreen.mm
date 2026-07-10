#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include "omegaWTK/Native/NativeScreen.h"

#include <cmath>
#include <memory>
#include <mutex>
#include <unordered_map>

@class OWTKDisplayLinkProxy;

namespace OmegaWTK::Native::Cocoa {
class CocoaDisplayLink;

unsigned screenIdFromNSScreen(NSScreen *screen);

}

@interface OWTKDisplayLinkProxy : NSObject
@property (nonatomic, assign) OmegaWTK::Native::Cocoa::CocoaDisplayLink *owner;
- (void)displayLinkDidFire:(CADisplayLink *)link;
@end

namespace OmegaWTK::Native::Cocoa {

/// CGDirectDisplayID — derived from NSScreen's deviceDescription.
unsigned screenIdFromNSScreen(NSScreen *screen) {
    if(screen == nil){
        return 0;
    }
    NSNumber *num = [[screen deviceDescription] objectForKey:@"NSScreenNumber"];
    return num ? (unsigned)[num unsignedIntValue] : 0;
}

NSScreen *findNSScreenById(unsigned id){
    for(NSScreen *s in [NSScreen screens]){
        if(screenIdFromNSScreen(s) == id){
            return s;
        }
    }
    return nil;
}

/// Translate an NSScreen rect (bottom-left origin, primary's frame
/// height defines the virtual top) into Composition::Rect (top-left
/// origin, matching Win32/GTK).
Composition::Rect rectFromNSRect(NSRect r, NSRect primaryFrame) {
    float topY = (float)(primaryFrame.size.height - (r.origin.y + r.size.height));
    return Composition::Rect{
        Composition::Point2D{(float)r.origin.x, topY},
        (float)r.size.width,
        (float)r.size.height};
}

NativeScreenDesc descFromNSScreen(NSScreen *screen, NSScreen *primary) {
    NativeScreenDesc d;
    if(screen == nil || primary == nil){
        return d;
    }
    NSRect primaryFrame = [primary frame];
    d.id           = screenIdFromNSScreen(screen);
    d.frame        = rectFromNSRect([screen frame],        primaryFrame);
    d.visibleFrame = rectFromNSRect([screen visibleFrame], primaryFrame);
    d.scaleFactor  = (float)[screen backingScaleFactor];
    d.isPrimary    = (screen == primary);

    // Refresh rate. NSScreen.maximumFramesPerSecond returns the
    // display's max rate (60 on standard, 120 on ProMotion). Available
    // macOS 12+.
    NSInteger fps = [screen maximumFramesPerSecond];
    d.refreshHz = fps > 0 ? (float)fps : 60.f;

    // VRR. maximumRefreshInterval is the longest valid refresh
    // interval; its reciprocal is the floor rate. The display is VRR
    // iff floor < max.
    NSTimeInterval maxInterval = [screen maximumRefreshInterval];
    if(maxInterval > 0.0 && std::isfinite(maxInterval)){
        float floorHz = (float)(1.0 / maxInterval);
        d.minRefreshHz = floorHz;
        // Half-Hz tolerance to avoid flagging fixed-rate displays that
        // happen to round-trip through floats slightly off.
        d.variableRefreshRate = floorHz + 0.5f < d.refreshHz;
    } else {
        d.minRefreshHz = d.refreshHz;
        d.variableRefreshRate = false;
    }
    return d;
}

/// Modern CADisplayLink wrapper (macOS 14+ via NSScreen). One per
/// CGDirectDisplayID, cached by the top-level displayLinkForScreen
/// factory. Single subscriber per NativeScreen.h's contract.
///
/// Threading: the link is added to the main run loop. The cb fires
/// on the main (UI) thread. Phase H's FramePacer is responsible for
/// keeping per-frame work off the main thread — the cb itself only
/// forwards a presentation timestamp, which is cheap.
class CocoaDisplayLink : public NativeDisplayLink {
public:
    explicit CocoaDisplayLink(CGDirectDisplayID displayID)
        : displayID_(displayID) {
        proxy_ = [[OWTKDisplayLinkProxy alloc] init];
        proxy_.owner = this;

        NSScreen *screen = findNSScreenById((unsigned)displayID_);
        if(screen == nil){
            screen = [NSScreen mainScreen];
        }
        if(screen != nil){
            // MRR target (no -fobjc-arc): displayLinkWithTarget: returns an
            // AUTORELEASED, non-owned CADisplayLink. We must retain it so its
            // lifetime is owned by this wrapper, NOT by the run loop (which
            // only holds it while subscribed) or the source NSScreen. Without
            // this +1, disconnecting the source display — e.g. entering
            // clamshell mode — releases the link out from under us and the
            // next subscribe()/unsubscribe() messages a freed object (crash).
            link_ = [[screen displayLinkWithTarget:proxy_
                                          selector:@selector(displayLinkDidFire:)] retain];
        }

        // Seed the nominal interval from the screen's max FPS so
        // expectedFrameIntervalNs() has a sensible answer before the
        // first tick lands.
        if(screen != nil){
            NSInteger fps = [screen maximumFramesPerSecond];
            if(fps > 0){
                nominalIntervalNs_ = (std::uint64_t)(1'000'000'000.0 / (double)fps);
            }
        }
    }

    ~CocoaDisplayLink() override {
        // MRR: invalidate stops the link and removes it from any run loop,
        // then release balances the +1 taken in the ctor. proxy_ was +1 from
        // alloc]init] and must be released too (it was previously leaked).
        if(link_ != nil){
            [link_ invalidate];
            [link_ release];
            link_ = nil;
        }
        if(proxy_ != nil){
            proxy_.owner = nullptr;
            [proxy_ release];
            proxy_ = nil;
        }
    }

    void subscribe(std::function<void(std::uint64_t, std::uint64_t)> cb) override {
        std::lock_guard<std::mutex> lk(mu_);
        callback_ = std::move(cb);
        if(link_ != nil && callback_ && !running_){
            [link_ addToRunLoop:[NSRunLoop mainRunLoop]
                         forMode:NSRunLoopCommonModes];
            running_ = true;
        }
    }

    void unsubscribe() override {
        std::lock_guard<std::mutex> lk(mu_);
        callback_ = nullptr;
        if(link_ != nil && running_){
            [link_ removeFromRunLoop:[NSRunLoop mainRunLoop]
                              forMode:NSRunLoopCommonModes];
            running_ = false;
        }
    }

    std::uint64_t expectedFrameIntervalNs() const override {
        std::lock_guard<std::mutex> lk(mu_);
        return nominalIntervalNs_;
    }

    /// Invoked from the Obj-C proxy on the run loop's thread.
    void onTick(std::uint64_t presentationNs, std::uint64_t intervalNs){
        std::function<void(std::uint64_t, std::uint64_t)> cb;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if(intervalNs != 0){
                nominalIntervalNs_ = intervalNs;
            }
            cb = callback_;
        }
        if(cb){
            cb(presentationNs, intervalNs);
        }
    }

private:
    CGDirectDisplayID displayID_;
    CADisplayLink *link_ = nil;
    OWTKDisplayLinkProxy *proxy_ = nil;
    std::function<void(std::uint64_t, std::uint64_t)> callback_;
    mutable std::mutex mu_;
    bool running_ = false;
    std::uint64_t nominalIntervalNs_ = 16'666'666ull;
};

/// Cache of weak refs keyed by CGDirectDisplayID, so consumers on
/// the same screen share one CADisplayLink. Strong ownership stays
/// with the caller's NativeDisplayLinkPtr; the cache entry expires
/// naturally when the last consumer drops it.
struct DisplayLinkCache {
    std::mutex mu;
    std::unordered_map<unsigned, std::weak_ptr<NativeDisplayLink>> entries;
};

DisplayLinkCache & displayLinkCache(){
    static DisplayLinkCache cache;
    return cache;
}

}

@implementation OWTKDisplayLinkProxy

- (void)displayLinkDidFire:(CADisplayLink *)link {
    OmegaWTK::Native::Cocoa::CocoaDisplayLink *owner = self.owner;
    if(owner == nullptr || link == nil){
        return;
    }
    // CADisplayLink.targetTimestamp: the time when the next frame is
    // expected to be displayed (CFAbsoluteTime, seconds).
    // CADisplayLink.duration: the interval between display updates.
    std::uint64_t presentationNs = (std::uint64_t)(link.targetTimestamp * 1'000'000'000.0);
    std::uint64_t intervalNs = 0;
    if(link.duration > 0.0){
        intervalNs = (std::uint64_t)(link.duration * 1'000'000'000.0);
    }
    owner->onTick(presentationNs, intervalNs);
}

@end

namespace OmegaWTK::Native {

OmegaCommon::Vector<NativeScreenDesc> enumerateScreens() {
    OmegaCommon::Vector<NativeScreenDesc> out;
    NSScreen *primary = [NSScreen mainScreen];
    NSArray<NSScreen *> *screens = [NSScreen screens];
    if(screens == nil){
        return out;
    }
    for(NSScreen *s in screens){
        out.push_back(Cocoa::descFromNSScreen(s, primary));
    }
    return out;
}

NativeScreenDesc primaryScreen() {
    NSScreen *primary = [NSScreen mainScreen];
    return Cocoa::descFromNSScreen(primary, primary);
}

NativeScreenDesc screenById(unsigned id) {
    NSScreen *primary = [NSScreen mainScreen];
    NSScreen *match = Cocoa::findNSScreenById(id);
    if(match != nil){
        return Cocoa::descFromNSScreen(match, primary);
    }
    return Cocoa::descFromNSScreen(primary, primary);
}

NativeDisplayLinkPtr displayLinkForScreen(const NativeScreenDesc & screen) {
    auto & cache = Cocoa::displayLinkCache();
    std::lock_guard<std::mutex> lk(cache.mu);
    auto it = cache.entries.find(screen.id);
    if(it != cache.entries.end()){
        if(auto live = it->second.lock()){
            return live;
        }
        cache.entries.erase(it);
    }
    auto link = std::make_shared<Cocoa::CocoaDisplayLink>((CGDirectDisplayID)screen.id);
    cache.entries.emplace(screen.id, link);
    return link;
}

}
