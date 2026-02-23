#import "NativePrivate/macos/CocoaItem.h"
#import "NativePrivate/macos/CocoaEvent.h"
#import "NativePrivate/macos/CocoaUtils.h"

#import <QuartzCore/QuartzCore.h>
#include <algorithm>
#include <cfloat>
#include <cmath>

@interface OmegaWTKCocoaView ()
@property (nonatomic) OmegaWTK::Native::Cocoa::CocoaItem *delegate;
@property(nonatomic,retain) NSTrackingArea *trackingArea;
@end


@implementation OmegaWTKCocoaView
- (instancetype)initWithFrame:(NSRect)rect delegate:(OmegaWTK::Native::Cocoa::CocoaItem *)delegate{
    if(self = [super initWithFrame:rect]){
        self.wantsLayer = YES;
        self.layer = [CALayer layer];
        self.layer.masksToBounds = NO;
        CGColorRef clearColor = CGColorCreateGenericRGB(0.f,0.f,0.f,0.f);
        self.layer.backgroundColor = clearColor;
        CGColorRelease(clearColor);
        self.layer.bounds = NSMakeRect(0.f,0.f,rect.size.width,rect.size.height);
        self.autoresizesSubviews = NO;
         self.layer.autoresizingMask = kCALayerHeightSizable | kCALayerWidthSizable;
        NSLog(@"Old Origin: { x:%f, y:%f}",self.layer.anchorPoint.x,self.layer.anchorPoint.y);
        self.layer.anchorPoint = CGPointMake(0.0,0.0);
        self.layer.position = CGPointMake(0.f,0.f);
        CGFloat startupScale = [NSScreen mainScreen].backingScaleFactor;
        if(startupScale <= 0.f || !std::isfinite(static_cast<double>(startupScale))){
            startupScale = 2.f;
        }
        self.layer.contentsScale = std::max(startupScale,static_cast<CGFloat>(2.f));
        // self.layer.contentsGravity = kCAGravityCenter;
        self.layer.magnificationFilter = kCAFilterLinear;
//        self.layerContentsRedrawPolicy = NSViewLayerContentsRedrawDuringViewResize;
//        self.layerContentsPlacement = NSViewLayerContentsPlacementCenter;
        NSLog(@"New Origin: { x:%f, y:%f}",self.layer.anchorPoint.x,self.layer.anchorPoint.y);
        _delegate = delegate;
        _trackingArea = [[NSTrackingArea alloc] initWithRect:rect options:NSTrackingMouseEnteredAndExited | NSTrackingCursorUpdate | NSTrackingActiveInKeyWindow owner:self userInfo:nil];
        [self addTrackingArea:_trackingArea];
    };
    return self;
};
-(void)emitEventIfPossible:(NSEvent *)event{
    if(self.delegate->hasEventEmitter()){
        self.delegate->sendEventToEmitter(OmegaWTK::Native::Cocoa::ns_event_to_omega_wtk_native_event(event));
    };
};

- (BOOL)acceptsFirstResponder {
    return YES;
};

- (void)mouseDown:(NSEvent *)event{
    [self emitEventIfPossible:event];
    [super mouseDown:event];
};
- (void)mouseUp:(NSEvent *)event{
    [self emitEventIfPossible:event];
    [super mouseUp:event];
};
-(BOOL)acceptsFirstMouse:(NSEvent *)event {
    return YES;
}
- (void)mouseEntered:(NSEvent *)event{
    [self emitEventIfPossible:event];
    [super mouseEntered:event];
};
- (void)mouseExited:(NSEvent *)event {
    [self emitEventIfPossible:event];
    [super mouseExited:event];
};

- (void)drawRect:(NSRect)dirtyRect{
    NSLog(@"NEVER CALL THIS FUNCTION!!!");
};
- (CALayer *)getCALayer {
    return (CALayer *)self.layer;
};

@end

@implementation OmegaWTKCocoaViewController{
    OmegaWTK::Native::Cocoa::CocoaItem *_delegate;
    Class _class;
    NSRect _rect;
}
- (instancetype)initWithFrame:(NSRect)rect delegate:(OmegaWTK::Native::Cocoa::CocoaItem *)delegate{
    if(self = [super init]){
        _rect = rect;
        _delegate = delegate;
    };
    return self;
};

-(void)setClass:(Class)cls {
    _class = cls;
}

- (void)loadView{
    NSLog(@"Load the View!");
    if(_class == [OmegaWTKCocoaView class]){
        self.view = [[_class alloc] initWithFrame:_rect delegate:_delegate];
    }
    else {
        self.view = [[_class alloc] initWithFrame:_rect];
    }
    // Child widget views are layout-driven by OmegaWTK.
    // Root window view sizing is configured when attached to the window.
    self.view.autoresizingMask = NSViewNotSizable;
};

- (void)viewDidLoad{
    NSLog(@"View Delegate is Ready!");
    _delegate->isReady = true;
};

// - (void)viewDidLayout{
//     [self.view setFrame:OmegaWTK::Native::Cocoa::core_rect_to_cg_rect(_delegate->rect)];
//     [self.view setBounds:CGRectMake(0.0,0.0,_delegate->rect.dimen.minWidth,_delegate->rect.dimen.minHeight)];
//     self.view.layer.position = self.view.frame.origin;
//     self.view.layer.frame = self.view.frame;
//     self.view.layer.bounds = self.view.bounds;
//     _delegate->layoutLayerTreeLimb();
// };

@end

@interface OmegaWTKCocoaScrollViewDelegate ()
@property (nonatomic,assign) OmegaWTK::Native::Cocoa::CocoaItem *delegate;
@property (nonatomic) NSPoint lastOrigin;
@end

namespace OmegaWTK::Native::Cocoa {

namespace {

constexpr CGFloat kMaxDrawableDimension = 16384.f;

static inline CGFloat safeScale(){
    CGFloat scale = [NSScreen mainScreen].backingScaleFactor;
    if(scale <= 0.f || !std::isfinite(static_cast<double>(scale))){
        scale = 2.f;
    }
    return std::max(scale,static_cast<CGFloat>(2.f));
}

static inline Core::Rect sanitizeNativeRect(const Core::Rect & candidate,const Core::Rect & fallback){
    Core::Rect saneFallback = fallback;
    if(!std::isfinite(saneFallback.pos.x)){
        saneFallback.pos.x = 0.f;
    }
    if(!std::isfinite(saneFallback.pos.y)){
        saneFallback.pos.y = 0.f;
    }

    const float maxPointDimension = static_cast<float>(kMaxDrawableDimension / safeScale());
    if(!std::isfinite(saneFallback.w) || saneFallback.w <= 0.f){
        saneFallback.w = 1.f;
    }
    if(!std::isfinite(saneFallback.h) || saneFallback.h <= 0.f){
        saneFallback.h = 1.f;
    }
    saneFallback.w = std::clamp(saneFallback.w,1.f,maxPointDimension);
    saneFallback.h = std::clamp(saneFallback.h,1.f,maxPointDimension);

    Core::Rect sanitized = candidate;
    if(!std::isfinite(sanitized.pos.x)){
        sanitized.pos.x = saneFallback.pos.x;
    }
    if(!std::isfinite(sanitized.pos.y)){
        sanitized.pos.y = saneFallback.pos.y;
    }
    if(!std::isfinite(sanitized.w) || sanitized.w <= 0.f){
        sanitized.w = saneFallback.w;
    }
    if(!std::isfinite(sanitized.h) || sanitized.h <= 0.f){
        sanitized.h = saneFallback.h;
    }
    sanitized.w = std::clamp(sanitized.w,1.f,maxPointDimension);
    sanitized.h = std::clamp(sanitized.h,1.f,maxPointDimension);
    return sanitized;
}

}

CocoaItem::CocoaItem(const Core::Rect & rect,
                     CocoaItem::Type _type,
                     SharedHandle<CocoaItem> parent):
rect(rect),
_ptr(nil),
cont(nil),
scrollView(nil),
scrollViewDelegate(nil),
type(_type),
isReady(false){
    this->rect = sanitizeNativeRect(this->rect,Core::Rect{Core::Position{0.f,0.f},1.f,1.f});
    if(type == View){
        cont = [[OmegaWTKCocoaViewController alloc] initWithFrame:core_rect_to_cg_rect(this->rect) delegate:this];
        [cont setClass:[OmegaWTKCocoaView class]];
        _ptr = (OmegaWTKCocoaView *)cont.view;
        if(parent != nullptr){
            parent->addChildNativeItem((NativeItemPtr)this);
        };
    };
    if(type == ScrollView){
        _ptr = nil;
        cont = [[OmegaWTKCocoaViewController alloc] initWithFrame:core_rect_to_cg_rect(this->rect) delegate:this];
        [cont setClass:[NSScrollView class]];
        scrollViewDelegate = [[OmegaWTKCocoaScrollViewDelegate alloc] initWithDelegate:this];
        scrollView = (NSScrollView *)cont.view;
        scrollView.autohidesScrollers = YES;
        scrollView.borderType = NSNoBorder;
        [scrollViewDelegate bindToScrollView:scrollView];
        if(parent != nullptr){
            parent->addChildNativeItem((NativeItemPtr)this);
        };
    };
};

// void CocoaItem::layoutLayerTreeLimb(){
//     layerTreelimb->layout();
// };

void * CocoaItem::getBinding(){
    return reinterpret_cast<void *>(this->cont);
};

void CocoaItem::enable(){
    if(_ptr != nil){
        if([_ptr isHidden] == YES){
            [_ptr setHidden:NO];
        }
    }
    else if(scrollView != nil){
        [scrollView setHidden:NO];
    }
};

void CocoaItem::disable(){
    if(_ptr != nil){
        if([_ptr isHidden] == NO){
            [_ptr setHidden:YES];
        }
    }
    else if(scrollView != nil){
        [scrollView setHidden:YES];
    }
};

void CocoaItem::resize(const Core::Rect &newRect){
    rect = sanitizeNativeRect(newRect,rect);
    CGRect r = core_rect_to_cg_rect(rect);
    if(_ptr != nil){
        [_ptr setFrame:r];
        [_ptr setBoundsOrigin:NSMakePoint(0,0)];
        [_ptr setBoundsSize:r.size];
        CALayer *layer = _ptr.layer;
        NSRect hostBounds = _ptr.bounds;
        hostBounds.origin = NSMakePoint(0.f,0.f);
        layer.frame = hostBounds;
        layer.position = CGPointMake(0.f,0.f);
        layer.bounds = hostBounds;
        CGFloat scale = safeScale();
        const CGFloat maxPointDimension = kMaxDrawableDimension / scale;
        hostBounds.size.width = MIN(MAX(hostBounds.size.width,1.f),maxPointDimension);
        hostBounds.size.height = MIN(MAX(hostBounds.size.height,1.f),maxPointDimension);
        layer.contentsScale = scale;
        if([layer isKindOfClass:[CAMetalLayer class]]){
            CAMetalLayer *metalLayer = (CAMetalLayer *)layer;
            metalLayer.contentsScale = scale;
            metalLayer.drawableSize = CGSizeMake(
                MIN(MAX(hostBounds.size.width * scale,1.f),kMaxDrawableDimension),
                MIN(MAX(hostBounds.size.height * scale,1.f),kMaxDrawableDimension));
        }
        NSArray<CALayer *> *subLayers = layer.sublayers;
        for(CALayer *subLayer in subLayers){
            subLayer.frame = hostBounds;
            subLayer.position = CGPointMake(0.f,0.f);
            subLayer.bounds = hostBounds;
            subLayer.contentsScale = scale;
            if([subLayer isKindOfClass:[CAMetalLayer class]]){
                CAMetalLayer *metalLayer = (CAMetalLayer *)subLayer;
                metalLayer.contentsScale = scale;
                metalLayer.drawableSize = CGSizeMake(
                    MIN(MAX(hostBounds.size.width * scale,1.f),kMaxDrawableDimension),
                    MIN(MAX(hostBounds.size.height * scale,1.f),kMaxDrawableDimension));
            }
        }
    }
    else {
        [scrollView setFrame:r];
    }
     
};

void CocoaItem::addChildNativeItem(NativeItemPtr native_item){
    auto cocoaview = std::dynamic_pointer_cast<CocoaItem>(native_item);
    if(cocoaview->_ptr != nil){
        [_ptr addSubview:cocoaview->_ptr];
    }
    else if(cocoaview->scrollView != nil){
        [_ptr addSubview:cocoaview->scrollView];
    }
};

void CocoaItem::removeChildNativeItem(NativeItemPtr native_item){
    auto cocoaview = std::dynamic_pointer_cast<CocoaItem>(native_item);
    if(cocoaview->_ptr != nil){
        [cocoaview->_ptr removeFromSuperview];
    }
    else if(cocoaview->scrollView != nil){
        [cocoaview->scrollView removeFromSuperview];
    }
};

void CocoaItem::setClippedView(NativeItemPtr nativeItem){
    auto cocoaItem = std::dynamic_pointer_cast<CocoaItem>(nativeItem);
    if(scrollView != nil && cocoaItem != nullptr){
        scrollView.documentView = cocoaItem->getView();
    }
};

void CocoaItem::toggleHorizontalScrollBar(bool &state){
    if(state)
        [scrollView setHasHorizontalScroller:YES];
    else 
        [scrollView setHasHorizontalScroller:NO];
};

void CocoaItem::toggleVerticalScrollBar(bool &state){
    if(state)
        [scrollView setHasVerticalScroller:YES];
    else 
        [scrollView setHasVerticalScroller:NO];
};

CocoaItem::~CocoaItem(){
    if(scrollViewDelegate != nil){
        if(scrollView != nil){
            [scrollViewDelegate unbindFromScrollView:scrollView];
        }
        [scrollViewDelegate release];
        scrollViewDelegate = nil;
    }
};

void CocoaItem::setNeedsDisplay(){
    _ptr.needsDisplay = YES;
};



};

@implementation OmegaWTKCocoaScrollViewDelegate
- (instancetype)initWithDelegate:(OmegaWTK::Native::Cocoa::CocoaItem *)delegate {
    self = [super init];
    if(self){
        _delegate = delegate;
        _lastOrigin = NSMakePoint(0.f,0.f);
    }
    return self;
}

- (void)bindToScrollView:(NSScrollView *)scrollView {
    if(scrollView == nil){
        return;
    }
    NSClipView *clipView = scrollView.contentView;
    if(clipView == nil){
        return;
    }
    [clipView setPostsBoundsChangedNotifications:YES];
    _lastOrigin = clipView.bounds.origin;
    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(onBoundsDidChange:)
                                                 name:NSViewBoundsDidChangeNotification
                                               object:clipView];
}

- (void)unbindFromScrollView:(NSScrollView *)scrollView {
    NSClipView *clipView = scrollView.contentView;
    [[NSNotificationCenter defaultCenter] removeObserver:self
                                                    name:NSViewBoundsDidChangeNotification
                                                  object:clipView];
}

- (void)onBoundsDidChange:(NSNotification *)notification {
    if(_delegate == nullptr || !_delegate->hasEventEmitter()){
        return;
    }
    NSClipView *clipView = (NSClipView *)notification.object;
    if(clipView == nil){
        return;
    }
    NSPoint origin = clipView.bounds.origin;
    float dx = static_cast<float>(origin.x - _lastOrigin.x);
    float dy = static_cast<float>(origin.y - _lastOrigin.y);
    constexpr float epsilon = FLT_EPSILON;
    if(std::fabs(dx) > epsilon){
        auto type = dx > 0.f ? OmegaWTK::Native::NativeEvent::ScrollRight : OmegaWTK::Native::NativeEvent::ScrollLeft;
        _delegate->sendEventToEmitter(OmegaWTK::Native::NativeEventPtr(
                new OmegaWTK::Native::NativeEvent(type,new OmegaWTK::Native::ScrollParams{dx,0.f})));
    }
    if(std::fabs(dy) > epsilon){
        auto type = dy > 0.f ? OmegaWTK::Native::NativeEvent::ScrollDown : OmegaWTK::Native::NativeEvent::ScrollUp;
        _delegate->sendEventToEmitter(OmegaWTK::Native::NativeEventPtr(
                new OmegaWTK::Native::NativeEvent(type,new OmegaWTK::Native::ScrollParams{0.f,dy})));
    }
    _lastOrigin = origin;
}

- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [super dealloc];
}
@end

namespace OmegaWTK::Native {
    NativeItemPtr make_native_item(Core::Rect rect,ItemType type,NativeItemPtr parent){
        Cocoa::CocoaItem::Type item_type;
        if(type == Default)
            item_type = Cocoa::CocoaItem::View;
        else if(type == ScrollItem){
            item_type = Cocoa::CocoaItem::ScrollView;
        }
        else {
            item_type = Cocoa::CocoaItem::View;
        }
        return (NativeItemPtr)new Cocoa::CocoaItem(rect,item_type,std::dynamic_pointer_cast<Cocoa::CocoaItem>(parent));
    };
}
