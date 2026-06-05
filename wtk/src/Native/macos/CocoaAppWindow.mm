#import "CocoaAppWindow.h"
#include "NativePrivate/macos/CocoaUtils.h"
#include "NativePrivate/macos/CocoaItem.h"
#include <cstdlib>


@interface OmegaWTKNativeCocoaAppWindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic,strong) NSWindow *window;
@property(nonatomic) OmegaWTK::Native::Cocoa::CocoaAppWindow * cppBinding;
@property(nonatomic) NSRect pendingResizeBounds;
@property(nonatomic) BOOL hasPendingResizeBounds;
@property(nonatomic) BOOL resizeDispatchQueued;
@property(nonatomic,assign) NSView *observedHostContentView;
@property(nonatomic) std::uint64_t nextResizeGeneration;
@property(nonatomic) std::uint64_t pendingResizeGeneration;
@property(nonatomic) std::uint64_t latestEmittedResizeGeneration;
- (void)attachHostContentViewObservers:(NSView *)hostView;
- (void)detachHostContentViewObservers;
- (void)hostContentViewGeometryDidChange:(NSNotification *)notification;
- (void)hostContentViewDidUpdateBounds:(NSRect)bounds;
@end

@interface OmegaWTKNativeCocoaAppWindowController : NSWindowController
- (instancetype)initWithRect:(NSRect) rect delegate:(id<NSWindowDelegate>) delegate;
@end

namespace OmegaWTK::Native::Cocoa {

namespace {
static inline bool traceResizeFlowEnabled(){
    static int enabled = -1;
    if(enabled == -1){
        auto value = OmegaCommon::getEnvVar("OMEGAWTK_TRACE_RESIZE_FLOW");
        enabled = (value.has_value() && !value->empty() && (*value)[0] != '0') ? 1 : 0;
    }
    return enabled == 1;
}
}


CocoaAppWindow::CocoaAppWindow(Composition::Rect & rect,NativeEventEmitter *emitter,const NativeScreenDesc *screen):NativeWindow(rect, emitter), currentNSCursor(nil){
    // §2.9 NativeScreen: `screen` carries the chosen target screen for
    // this window. AppKit already picks the right backing scale from
    // the absolute rect we pass to `initWithRect:` (NSWindow looks up
    // its NSScreen from the frame), so there is no per-state seeding
    // to do on macOS — the parameter is accepted for source-symmetry
    // with GTK / Win32 and for any future macOS-specific seeding work.
    (void)screen;

    windowDelegate = [[OmegaWTKNativeCocoaAppWindowDelegate alloc] init];
    windowController = [[OmegaWTKNativeCocoaAppWindowController alloc] initWithRect:core_rect_to_cg_rect(rect) delegate:windowDelegate];
    windowDelegate.window = windowController.window;
    windowDelegate.cppBinding = this;

    rootView = std::dynamic_pointer_cast<CocoaItem>(Native::make_native_item(rect));

    [windowController.window setContentViewController:((NSViewController *)rootView->getBinding())];
    // The view controller's loadView defaults the root view to NSViewNotSizable
    // because that policy is correct for child native items embedded in the
    // virtual tree (OmegaWTK lays them out). When the same view is installed
    // as the window's contentView it must follow the window so that frame
    // change notifications fire and the live-resize pipeline sees real bounds.
    windowController.window.contentView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [windowDelegate attachHostContentViewObservers:windowController.window.contentView];
    lastKnownBackingScale = (float)[windowController.window backingScaleFactor];
};

NativeEventEmitter * CocoaAppWindow::getEmitter() {
    return eventEmitter();
};

void CocoaAppWindow::requestFrameFlush(){
    // Widget-View-Paint-Lifecycle-Plan Tier A: coalesce a burst of
    // frame requests into one flush on the next run-loop turn. Mirrors
    // the resize-coalescing block in the window delegate.
    // kCFRunLoopCommonModes so the flush fires during live resize
    // (NSEventTrackingRunLoopMode), not only NSDefaultRunLoopMode.
    if(frameFlushQueued_){
        return;
    }
    frameFlushQueued_ = true;
    CocoaAppWindow *self = this;
    CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{
        self->frameFlushQueued_ = false;
        if(self->frameFlushCallback_){
            self->frameFlushCallback_();
        }
    });
    CFRunLoopWakeUp(CFRunLoopGetMain());
}

void CocoaAppWindow::disable(){
    if([windowController.window isVisible] == YES){
        [windowController.window orderOut:nil];
    };
};

void CocoaAppWindow::enable(){
    if([windowController.window isVisible] == NO){
        [windowController.window makeKeyAndOrderFront:nil];
    };
    // NativeWindow-Ready-Signal-Plan: the window is now on screen with
    // its CAMetalLayer wired up. Idempotent — handleFirstRealize
    // no-ops if initialDisplay or orderFront already fired it.
    handleFirstRealize();
};

void CocoaAppWindow::setMenu(NM menu) {
     NSMenu * cocoa_menu = (NSMenu *)menu->getNativeBinding();
     [windowController.window setMenu:cocoa_menu];
}

void CocoaAppWindow::setTitle(OmegaCommon::StrRef title){
    windowController.window.styleMask |= NSWindowStyleMaskTitled;
    [windowController.window setTitleVisibility:NSWindowTitleVisible];
    NSString *windowTitle = [[NSString alloc] initWithBytes:title.data()
                                                     length:title.size()
                                                   encoding:NSUTF8StringEncoding];
    if(windowTitle == nil){
        windowTitle = [[NSString alloc] initWithUTF8String:""];
    }
    [windowController.window setTitle:windowTitle];
    [windowTitle release];
}

void CocoaAppWindow::setEnableWindowHeader(bool &enable) {
    [windowController.window setTitleVisibility:NSWindowTitleHidden];
}

void CocoaAppWindow::initialDisplay(){
    [windowController.window center];
    // NSView *rootView = [[NSView alloc] initWithFrame:window.frame];
    // rootView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    // [window setContentView:rootView];
    // // [window layoutIfNeeded];
    // // if(menu){
    // //     NSMenu * windowMenu = (NSMenu *)menu->getNativeBinding();
    // //     [window setMenu:windowMenu];
    // //     [NSApp setMainMenu:windowMenu];
    // // };
    NSLog(@"Display Window");
    NSMenu *windowMenu = windowController.window.menu;
    if(windowMenu != nil){
        NSApp.mainMenu = windowMenu;
    }
    [windowController showWindow:windowDelegate];
    [windowController.window makeKeyAndOrderFront:nil];
    NSLog(@"IS Visible :%d",[windowController.window isVisible]);
    // NativeWindow-Ready-Signal-Plan step 4: the standard "the window
    // is now on screen" entry point. Flips isNativeReady() to true and
    // dispatches any queued onFirstRealize subscribers.
    handleFirstRealize();
};

void CocoaAppWindow::close(){
//    if([windowController.window isVisible] == YES)
//        [windowController.window close];
};

NativeItemPtr CocoaAppWindow::getRootView() {
    return std::static_pointer_cast<NativeItem>(rootView);
}

 __strong NSWindow *CocoaAppWindow::getWindow(){
     return windowController.window;
 };

namespace {
NSCursor *cursorForShape(CursorShape shape){
    switch(shape){
        case CursorShape::Arrow:           return [NSCursor arrowCursor];
        case CursorShape::IBeam:           return [NSCursor IBeamCursor];
        case CursorShape::Crosshair:       return [NSCursor crosshairCursor];
        case CursorShape::PointingHand:    return [NSCursor pointingHandCursor];
        case CursorShape::ResizeLeftRight: return [NSCursor resizeLeftRightCursor];
        case CursorShape::ResizeUpDown:    return [NSCursor resizeUpDownCursor];
        case CursorShape::ResizeAll:       return [NSCursor closedHandCursor];
        case CursorShape::NotAllowed:      return [NSCursor operationNotAllowedCursor];
        case CursorShape::Wait:            return [NSCursor arrowCursor];
        case CursorShape::Custom:          return [NSCursor arrowCursor];
    }
    return [NSCursor arrowCursor];
}
}

void CocoaAppWindow::minimize(){
    [windowController.window miniaturize:nil];
}
void CocoaAppWindow::maximize(){
    if(![windowController.window isZoomed]){
        [windowController.window zoom:nil];
    }
}
void CocoaAppWindow::restore(){
    if([windowController.window isMiniaturized]){
        [windowController.window deminiaturize:nil];
    } else if([windowController.window isZoomed]){
        [windowController.window zoom:nil];
    }
}
void CocoaAppWindow::toggleFullscreen(){
    [windowController.window toggleFullScreen:nil];
}
bool CocoaAppWindow::isMinimized() const {
    return [windowController.window isMiniaturized] == YES;
}
bool CocoaAppWindow::isMaximized() const {
    return [windowController.window isZoomed] == YES;
}
bool CocoaAppWindow::isFullscreen() const {
    return ([windowController.window styleMask] & NSWindowStyleMaskFullScreen) != 0;
}
bool CocoaAppWindow::isVisible() const {
    return [windowController.window isVisible] == YES;
}
Composition::Rect CocoaAppWindow::getRect() const {
    NSRect frame = [windowController.window frame];
    NSScreen *screen = [windowController.window screen] ?: [NSScreen mainScreen];
    CGFloat screenH = screen ? screen.frame.size.height : 0.0;
    // NSScreen origin is bottom-left; WTK pos.y is the top edge in top-left
    // screen space, so map frame's bottom-edge to a top-edge.
    float topLeftY = (float)(screenH - frame.origin.y - frame.size.height);
    return Composition::Rect{Composition::Point2D{(float)frame.origin.x, topLeftY},(float)frame.size.width,(float)frame.size.height};
}
void CocoaAppWindow::setRect(const Composition::Rect & r){
    NSScreen *screen = [windowController.window screen] ?: [NSScreen mainScreen];
    CGFloat screenH = screen ? screen.frame.size.height : 0.0;
    CGFloat bottomLeftY = screenH - (CGFloat)r.pos.y - (CGFloat)r.h;
    NSRect frame = NSMakeRect(r.pos.x, bottomLeftY, r.w, r.h);
    [windowController.window setFrame:frame display:YES];
    rect = r;
}
// §2.9: scaleFactor() now lives at NativeWindow as a base forwarder to
// currentScreen().scaleFactor. The NSWindow's backingScaleFactor still
// drives the WindowScaleFactorChanged emit (see
// -windowDidChangeBackingProperties:) but the getter is unified.
void CocoaAppWindow::setMinSize(float w, float h){
    [windowController.window setContentMinSize:NSMakeSize(w,h)];
}
void CocoaAppWindow::setMaxSize(float w, float h){
    [windowController.window setContentMaxSize:NSMakeSize(w,h)];
}
void CocoaAppWindow::setResizable(bool resizable){
    NSWindow *w = windowController.window;
    NSWindowStyleMask mask = [w styleMask];
    if(resizable){
        mask |= NSWindowStyleMaskResizable;
    } else {
        mask &= ~NSWindowStyleMaskResizable;
    }
    [w setStyleMask:mask];
}
// NativeWindow-Ready-Signal-Plan note: orderFront() also brings the
// window on screen, so it covers the case where a caller bypassed
// initialDisplay/enable and goes straight here. Idempotent via
// handleFirstRealize's firstRealizeFired_ guard.
void CocoaAppWindow::orderFront(){
    [windowController.window orderFront:nil];
    handleFirstRealize();
}
void CocoaAppWindow::orderBack(){
    [windowController.window orderBack:nil];
}
void CocoaAppWindow::setOpacity(float alpha){
    [windowController.window setAlphaValue:alpha];
}
float CocoaAppWindow::getOpacity() const {
    return (float)[windowController.window alphaValue];
}
void CocoaAppWindow::setCursorShape(CursorShape shape){
    currentCursorShape = shape;
    currentNSCursor = cursorForShape(shape);
    if([windowController.window isKeyWindow]){
        [currentNSCursor set];
    }
}
bool CocoaAppWindow::isKeyWindow() const {
    return [windowController.window isKeyWindow] == YES;
}
void CocoaAppWindow::becomeKeyWindow(){
    [windowController.window makeKeyAndOrderFront:nil];
}
void CocoaAppWindow::applyCursor(){
    if(currentNSCursor != nil){
        [currentNSCursor set];
    }
}
void CocoaAppWindow::notifyBackingScaleChanged(float oldScale, float newScale){
    if(!hasEventEmitter()){
        return;
    }
    auto *params = new WindowScaleFactorChangedParams{oldScale, newScale, {}};
    NativeEventPtr ev(new NativeEvent(NativeEvent::WindowScaleFactorChanged, params));
    eventEmitter()->emit(ev);
}

// ---- NativeWindow-Ready-Signal-Plan step 4 overrides ----

bool CocoaAppWindow::isNativeReady() const {
    return nativeReady_.load(std::memory_order_acquire);
}

void CocoaAppWindow::onFirstRealize(std::function<void()> cb){
    if(!cb){
        return;
    }
    {
        std::lock_guard<std::mutex> lk(realizeCallbacksMutex_);
        if(!firstRealizeFired_){
            firstRealizeSubscribers_.push_back(std::move(cb));
            return;
        }
    }
    // Post-realize fast path: fire synchronously on the calling thread.
    // cb was not moved on this path because the if-branch returned early.
    cb();
}

void CocoaAppWindow::onRealize(std::function<void()> cb){
    if(!cb){
        return;
    }
    // Sticky semantics: no synchronous-replay path. The callback fires
    // only on future re-realize transitions (handleReRealize, triggered
    // from windowDidChangeBackingProperties:).
    std::lock_guard<std::mutex> lk(realizeCallbacksMutex_);
    realizeSubscribers_.push_back(std::move(cb));
}

void CocoaAppWindow::handleFirstRealize(){
    std::vector<std::function<void()>> firstCallbacks;
    {
        std::lock_guard<std::mutex> lk(realizeCallbacksMutex_);
        nativeReady_.store(true, std::memory_order_release);
        if(firstRealizeFired_){
            return;  // idempotent — initialDisplay/enable/orderFront may all call this
        }
        firstRealizeFired_ = true;
        // Drain + free storage: firstRealize subscribers fire exactly
        // once per NativeWindow lifetime.
        firstCallbacks.swap(firstRealizeSubscribers_);
    }
    for(auto & cb : firstCallbacks){
        if(cb) cb();
    }
}

void CocoaAppWindow::handleReRealize(){
    std::vector<std::function<void()>> reCallbacks;
    {
        std::lock_guard<std::mutex> lk(realizeCallbacksMutex_);
        // Sticky semantics: copy the subscriber list so it stays intact
        // for subsequent fires. nativeReady_ stays true through a
        // re-realize (a backing-scale change does not invalidate the
        // surface itself; only the scale of the rasterization).
        reCallbacks = realizeSubscribers_;
    }
    for(auto & cb : reCallbacks){
        if(cb) cb();
    }
}

};

@implementation OmegaWTKNativeCocoaAppWindowDelegate

-(void)emitIfPossible:(OmegaWTK::Native::NativeEventPtr)event{
    if(self.cppBinding->hasEventEmitter()){
        self.cppBinding->getEmitter()->emit(event);
    };
};

-(void)emitResizeBoundsIfPossible:(NSRect)bounds generation:(std::uint64_t)generation{
    if(bounds.size.width <= 0.f || bounds.size.height <= 0.f){
        return;
    }
    auto *params = new OmegaWTK::Native::WindowWillResize(
            OmegaWTK::Composition::Rect
                    {0.f,
                     0.f,
                     (float)bounds.size.width,
                     (float)bounds.size.height},
            generation
    );
    OmegaWTK::Native::NativeEventPtr event(
            new OmegaWTK::Native::NativeEvent(
                    OmegaWTK::Native::NativeEvent::WindowWillResize,
                    params));
    [self emitIfPossible:event];
}

-(void)emitResizeBeginBoundsIfPossible:(NSRect)bounds generation:(std::uint64_t)generation{
    if(bounds.size.width <= 0.f || bounds.size.height <= 0.f){
        return;
    }
    auto *params = new OmegaWTK::Native::WindowWillResize(
            OmegaWTK::Composition::Rect
                    {0.f,
                     0.f,
                     (float)bounds.size.width,
                     (float)bounds.size.height},
            generation
    );
    OmegaWTK::Native::NativeEventPtr event(
            new OmegaWTK::Native::NativeEvent(
                    OmegaWTK::Native::NativeEvent::WindowWillStartResize,
                    params));
    [self emitIfPossible:event];
}

-(void)queueResizeBounds:(NSRect)bounds{
    const std::uint64_t generation = ++self.nextResizeGeneration;
    if(OmegaWTK::Native::Cocoa::traceResizeFlowEnabled()){
        NSLog(@"[OmegaWTKResize] queue gen=%llu bounds={%.2f,%.2f %.2fx%.2f} queued=%d pending=%d latestEmitted=%llu",
              static_cast<unsigned long long>(generation),
              bounds.origin.x,bounds.origin.y,bounds.size.width,bounds.size.height,
              self.resizeDispatchQueued,self.hasPendingResizeBounds,
              static_cast<unsigned long long>(self.latestEmittedResizeGeneration));
    }
    self.pendingResizeBounds = bounds;
    self.pendingResizeGeneration = generation;
    self.hasPendingResizeBounds = YES;
    if(self.resizeDispatchQueued){
        return;
    }
    self.resizeDispatchQueued = YES;
    OmegaWTKNativeCocoaAppWindowDelegate *delegate = self;
    // Use CFRunLoopPerformBlock with kCFRunLoopCommonModes so that the
    // block fires during live resize (NSEventTrackingRunLoopMode).
    // dispatch_async targets NSDefaultRunLoopMode only, which is suspended
    // while the user drags a window edge.
    CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{
        delegate.resizeDispatchQueued = NO;
        if(!delegate.hasPendingResizeBounds){
            return;
        }
        NSRect pending = delegate.pendingResizeBounds;
        const std::uint64_t pendingGeneration = delegate.pendingResizeGeneration;
        delegate.hasPendingResizeBounds = NO;
        delegate.pendingResizeGeneration = 0;
        if(pendingGeneration <= delegate.latestEmittedResizeGeneration){
            if(OmegaWTK::Native::Cocoa::traceResizeFlowEnabled()){
                NSLog(@"[OmegaWTKResize] drop stale queued gen=%llu latestEmitted=%llu",
                      static_cast<unsigned long long>(pendingGeneration),
                      static_cast<unsigned long long>(delegate.latestEmittedResizeGeneration));
            }
            return;
        }
        delegate.latestEmittedResizeGeneration = pendingGeneration;
        if(OmegaWTK::Native::Cocoa::traceResizeFlowEnabled()){
            NSLog(@"[OmegaWTKResize] emit queued gen=%llu bounds={%.2f,%.2f %.2fx%.2f}",
                  static_cast<unsigned long long>(pendingGeneration),
                  pending.origin.x,pending.origin.y,pending.size.width,pending.size.height);
        }
        [delegate emitResizeBoundsIfPossible:pending generation:pendingGeneration];
    });
    CFRunLoopWakeUp(CFRunLoopGetMain());
}

-(void)attachHostContentViewObservers:(NSView *)hostView{
    if(hostView == nil){
        return;
    }
    if(self.observedHostContentView == hostView){
        [hostView setPostsFrameChangedNotifications:YES];
        [hostView setPostsBoundsChangedNotifications:YES];
        return;
    }
    [self detachHostContentViewObservers];
    self.observedHostContentView = hostView;
    [hostView setAutoresizesSubviews:NO];
    [hostView setPostsFrameChangedNotifications:YES];
    [hostView setPostsBoundsChangedNotifications:YES];
    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(hostContentViewGeometryDidChange:)
                                                 name:NSViewFrameDidChangeNotification
                                               object:hostView];
    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(hostContentViewGeometryDidChange:)
                                                 name:NSViewBoundsDidChangeNotification
                                               object:hostView];
}

-(void)detachHostContentViewObservers{
    if(self.observedHostContentView == nil){
        return;
    }
    [[NSNotificationCenter defaultCenter] removeObserver:self
                                                    name:NSViewFrameDidChangeNotification
                                                  object:self.observedHostContentView];
    [[NSNotificationCenter defaultCenter] removeObserver:self
                                                    name:NSViewBoundsDidChangeNotification
                                                  object:self.observedHostContentView];
    self.observedHostContentView = nil;
}

-(void)hostContentViewGeometryDidChange:(NSNotification *)notification{
    NSView *view = (NSView *)notification.object;
    if(view == nil){
        return;
    }
    if(OmegaWTK::Native::Cocoa::traceResizeFlowEnabled()){
        NSLog(@"[OmegaWTKResize] host-content geometry change frame={%.2f,%.2f %.2fx%.2f} bounds={%.2f,%.2f %.2fx%.2f}",
              view.frame.origin.x,view.frame.origin.y,view.frame.size.width,view.frame.size.height,
              view.bounds.origin.x,view.bounds.origin.y,view.bounds.size.width,view.bounds.size.height);
    }
    [self hostContentViewDidUpdateBounds:view.bounds];
}

-(void)hostContentViewDidUpdateBounds:(NSRect)bounds{
    [self queueResizeBounds:bounds];
}

-(NSRect)contentBoundsForWindow:(NSWindow *)window{
    if(window == nil){
        return NSZeroRect;
    }
    if(window.contentView == nil){
        return NSZeroRect;
    }
    return window.contentView.bounds;
}

-(void)windowWillClose:(NSNotification *)notification {
    [self detachHostContentViewObservers];
    OmegaWTK::Native::NativeEventPtr event(new OmegaWTK::Native::NativeEvent(OmegaWTK::Native::NativeEvent::WindowWillClose,nullptr));
    [self emitIfPossible:event];
    
};

-(NSSize)windowWillResize:(NSWindow *)sender toSize:(NSSize)frameSize {
    if(sender == nil){
        return frameSize;
    }
    if(self.observedHostContentView != nil){
        if(OmegaWTK::Native::Cocoa::traceResizeFlowEnabled()){
            NSLog(@"[OmegaWTKResize] windowWillResize skipped (host-view authoritative) size=%.2fx%.2f",
                  frameSize.width,frameSize.height);
        }
        return frameSize;
    }
    NSRect projectedFrame = sender.frame;
    projectedFrame.size = frameSize;
    NSRect projectedContent = [sender contentRectForFrameRect:projectedFrame];
    [self queueResizeBounds:projectedContent];
    return frameSize;
}

-(void)windowWillStartLiveResize:(NSNotification *)notification {
    NSWindow *window = (NSWindow *)notification.object;
    if(window == nil){
        window = self.window;
    }
    NSRect bounds = [self contentBoundsForWindow:window];
    if(NSEqualRects(bounds,NSZeroRect)){
        return;
    }
    const std::uint64_t generation = ++self.nextResizeGeneration;
    [self emitResizeBeginBoundsIfPossible:bounds generation:generation];
}

-(void)windowDidResize:(NSNotification *)notification {
    NSWindow *window = (NSWindow *)notification.object;
    if(window == nil){
        window = self.window;
    }
    if(self.observedHostContentView != nil){
        if(OmegaWTK::Native::Cocoa::traceResizeFlowEnabled()){
            NSRect current = [self contentBoundsForWindow:window];
            NSLog(@"[OmegaWTKResize] windowDidResize skipped (host-view authoritative) bounds={%.2f,%.2f %.2fx%.2f}",
                  current.origin.x,current.origin.y,current.size.width,current.size.height);
        }
        return;
    }
    NSRect bounds = [self contentBoundsForWindow:window];
    if(NSEqualRects(bounds,NSZeroRect)){
        return;
    }
    NSLog(@"Window BOUNDS: {x:%f,y:%f,w:%f,h:%f}",bounds.origin.x,bounds.origin.y,bounds.size.width,bounds.size.height);
    [self queueResizeBounds:bounds];
};

-(void)windowDidChangeBackingProperties:(NSNotification *)notification {
    NSWindow *window = (NSWindow *)notification.object;
    if(window == nil){
        window = self.window;
    }
    if(window == nil || self.cppBinding == nullptr){
        return;
    }
    NSNumber *oldKey = notification.userInfo[NSBackingPropertyOldScaleFactorKey];
    float oldScale = oldKey != nil ? oldKey.floatValue : 1.f;
    float newScale = (float)[window backingScaleFactor];
    self.cppBinding->notifyBackingScaleChanged(oldScale, newScale);
    // NativeWindow-Ready-Signal-Plan step 4: backing-scale change is
    // the primary re-realize trigger on macOS (display change, monitor
    // swap). Layer re-host on space/fullscreen transitions is a future
    // follow-up — when wired, it routes through here too.
    self.cppBinding->handleReRealize();
}

-(void)windowDidEndLiveResize:(NSNotification *)notification
{
    NSWindow *window = (NSWindow *)notification.object;
    if(window == nil){
        window = self.window;
    }
    NSRect bounds = [self contentBoundsForWindow:window];
    if(!NSEqualRects(bounds,NSZeroRect)){
        const std::uint64_t generation = ++self.nextResizeGeneration;
        self.pendingResizeBounds = bounds;
        self.pendingResizeGeneration = generation;
        self.hasPendingResizeBounds = NO;
        self.resizeDispatchQueued = NO;
        if(generation > self.latestEmittedResizeGeneration){
            self.latestEmittedResizeGeneration = generation;
            [self emitResizeBoundsIfPossible:bounds generation:generation];
        } else if(OmegaWTK::Native::Cocoa::traceResizeFlowEnabled()){
            NSLog(@"[OmegaWTKResize] drop stale end-resize gen=%llu latestEmitted=%llu",
                  static_cast<unsigned long long>(generation),
                  static_cast<unsigned long long>(self.latestEmittedResizeGeneration));
        }
    }
    OmegaWTK::Native::NativeEventPtr event(
            new OmegaWTK::Native::NativeEvent(
                    OmegaWTK::Native::NativeEvent::WindowHasFinishedResize,
                    nullptr));
    [self emitIfPossible:event];
};

@end

@implementation OmegaWTKNativeCocoaAppWindowController
- (instancetype)initWithRect:(NSRect) rect delegate:(id<NSWindowDelegate>) delegate
{
    self = [super initWithWindow:[[NSWindow alloc] initWithContentRect:rect styleMask: NSWindowStyleMaskTitled | NSWindowStyleMaskBorderless | NSWindowStyleMaskFullSizeContentView | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable backing:NSBackingStoreBuffered defer:NO]];
    if (self) {
        NSWindow *window = self.window;
        self.window.titlebarAppearsTransparent = YES;
        window.delegate = delegate;
    }
    return self;
}

@end

namespace OmegaWTK::Native {
    NWH make_native_window(Composition::Rect & rect,NativeEventEmitter *emitter,const NativeScreenDesc *screen){
        return (NWH)new Cocoa::CocoaAppWindow(rect,emitter,screen);
    };
}
