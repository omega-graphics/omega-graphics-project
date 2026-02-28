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
        const char *value = std::getenv("OMEGAWTK_TRACE_RESIZE_FLOW");
        enabled = (value != nullptr && value[0] != '\0' && value[0] != '0') ? 1 : 0;
    }
    return enabled == 1;
}
}


CocoaAppWindow::CocoaAppWindow(Core::Rect & rect,NativeEventEmitter *emitter):NativeWindow(rect){
    eventEmitter = emitter;

    windowDelegate = [[OmegaWTKNativeCocoaAppWindowDelegate alloc] init];
    windowController = [[OmegaWTKNativeCocoaAppWindowController alloc] initWithRect:core_rect_to_cg_rect(rect) delegate:windowDelegate];
    windowDelegate.window = windowController.window;
    windowDelegate.cppBinding = this;

    rootView = std::dynamic_pointer_cast<CocoaItem>(Native::make_native_item(rect));

    [windowController.window setContentViewController:((NSViewController *)rootView->getBinding())];
    [windowDelegate attachHostContentViewObservers:windowController.window.contentView];
};

NativeEventEmitter * CocoaAppWindow::getEmitter() {
    return eventEmitter;
};

void CocoaAppWindow::disable(){
    if([windowController.window isVisible] == YES){
        [windowController.window orderOut:nil];
    };
};

void CocoaAppWindow::enable(){
    if([windowController.window isVisible] == NO){
        [windowController.window makeKeyAndOrderFront:nil];
    };
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

void CocoaAppWindow::addNativeItem(NativeItemPtr item){
        // auto *cocoaitem = (CocoaItem *)item;
        NSViewController *viewC = (NSViewController *)item->getBinding();
        NSView *contentView = windowController.window.contentView;
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        [contentView setAutoresizesSubviews:NO];
        viewC.view.frame = contentView.bounds;
        viewC.view.translatesAutoresizingMaskIntoConstraints = NO;
        viewC.view.autoresizingMask = NSViewNotSizable;
        viewC.view.hidden = NO;
        [windowController.window.contentViewController addChildViewController:viewC];
        [contentView addSubview:viewC.view];
        [CATransaction commit];
        NSLog(@"Added Native Item View: frame={%.1f,%.1f,%.1f,%.1f} bounds={%.1f,%.1f,%.1f,%.1f} hidden=%d subviews=%lu",
              viewC.view.frame.origin.x,viewC.view.frame.origin.y,
              viewC.view.frame.size.width,viewC.view.frame.size.height,
              viewC.view.bounds.origin.x,viewC.view.bounds.origin.y,
              viewC.view.bounds.size.width,viewC.view.bounds.size.height,
              viewC.view.isHidden,
              (unsigned long)contentView.subviews.count);
};

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
            OmegaWTK::Core::Rect
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
            OmegaWTK::Core::Rect
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
    dispatch_async(dispatch_get_main_queue(), ^{
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
    NWH make_native_window(Core::Rect & rect,NativeEventEmitter *emitter){
        return (NWH)new Cocoa::CocoaAppWindow(rect,emitter);
    };
}
