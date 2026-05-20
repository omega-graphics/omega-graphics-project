#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#include "omegaWTK/Native/NativeItem.h"
#include <cmath>

#ifndef OMEGAWTK_NATIVE_MACOS_COCOA_ITEM_H
#define OMEGAWTK_NATIVE_MACOS_COCOA_ITEM_H

namespace OmegaWTK::Native {
namespace Cocoa {
class CocoaItem;

}
}


@interface OmegaWTKCocoaView : NSView
-(instancetype) initWithFrame:(NSRect) rect delegate:(OmegaWTK::Native::Cocoa::CocoaItem *)delegate;
-(CALayer *) getCALayer;
@end

@interface OmegaWTKCocoaViewController : NSViewController
-(instancetype) initWithFrame:(NSRect) rect delegate:(OmegaWTK::Native::Cocoa::CocoaItem *) delegate;
-(void)setClass:(Class)cls;
@end

@interface OmegaWTKCocoaScrollViewDelegate : NSObject
-(instancetype)initWithDelegate:(OmegaWTK::Native::Cocoa::CocoaItem *)delegate;
-(void)bindToScrollView:(NSScrollView *)scrollView;
-(void)unbindFromScrollView:(NSScrollView *)scrollView;
@end

@class CALayer;


namespace OmegaWTK::Native {

namespace Cocoa {

class CocoaItem : public NativeItem {
    OmegaWTKCocoaView * _ptr;
    OmegaWTKCocoaViewController *cont;
    NSScrollView *scrollView;
    OmegaWTKCocoaScrollViewDelegate *scrollViewDelegate;
    CAMetalLayer *metalLayer_ = nil;
    friend class CocoaEventHandler;
    void enable() override;
    void disable() override;
    void addChildNativeItem(NativeItemPtr nativeItem) override;
    void removeChildNativeItem(NativeItemPtr nativeItem) override;

    void setClippedView(NativeItemPtr clippedView) override;
    void toggleHorizontalScrollBar(bool &state) override;
    void toggleVerticalScrollBar(bool &state) override;
public:
    Composition::Rect rect;
    typedef enum : OPT_PARAM {
        View,
        ScrollView
    } Type;
private:
    Type type;
public:
    bool isReady;
    void resize(const Composition::Rect &newRect) override;
    CALayer *getLayer(){ return [_ptr getCALayer];};
    NSView *getView(){ return _ptr != nil ? (NSView *)_ptr : (NSView *)scrollView; };
    void setRootLayer(CALayer *layer){
        if(_ptr != nil && layer != nil){
            NSDictionary *noActions = @{
                @"bounds":[NSNull null],
                @"position":[NSNull null],
                @"frame":[NSNull null],
                @"sublayers":[NSNull null],
                @"contents":[NSNull null],
                @"transform":[NSNull null]
            };
            [CATransaction begin];
            [CATransaction setDisableActions:YES];

            CALayer *hostLayer = _ptr.layer;
            NSRect hostBounds = _ptr.bounds;
            if(hostBounds.size.width <= 0.f || hostBounds.size.height <= 0.f){
                hostBounds = _ptr.frame;
            }
            hostBounds.origin = NSMakePoint(0.f,0.f);

            CGFloat scale = [NSScreen mainScreen].backingScaleFactor;
            if(scale <= 0.f || !std::isfinite(static_cast<double>(scale))){
                scale = 2.f;
            }
            scale = MAX(scale,2.f);
            const CGFloat maxDrawableDimension = 16384.f;
            const CGFloat maxPointDimension = maxDrawableDimension / scale;
            hostBounds.size.width = MIN(MAX(hostBounds.size.width,1.f),maxPointDimension);
            hostBounds.size.height = MIN(MAX(hostBounds.size.height,1.f),maxPointDimension);

            layer.actions = noActions;
            // The root present layer is the sole sublayer of the
            // contentView's backing layer. Let Core Animation track
            // the host bounds via the autoresize mask so resize-time
            // work only has to refresh drawableSize / contentsScale.
            // The virtual View/Widget tree is unaffected — this mask
            // only governs this single CAMetalLayer.
            layer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
            layer.masksToBounds = NO;
            layer.contentsScale = scale;
            layer.hidden = NO;
            layer.anchorPoint = CGPointMake(0.f,0.f);
            layer.position = CGPointMake(0.f,0.f);
            layer.bounds = hostBounds;
            layer.frame = hostBounds;

            if([layer isKindOfClass:[CAMetalLayer class]]){
                CAMetalLayer *metalLayer = (CAMetalLayer *)layer;
                metalLayer.opaque = NO;
                metalLayer.framebufferOnly = NO;
                metalLayer.presentsWithTransaction = NO;
                CGColorRef clearColor = CGColorCreateGenericRGB(0.f,0.f,0.f,0.f);
                metalLayer.backgroundColor = clearColor;
                CGColorRelease(clearColor);
                metalLayer.drawableSize = CGSizeMake(
                        MIN(MAX(hostBounds.size.width * scale,1.f),maxDrawableDimension),
                        MIN(MAX(hostBounds.size.height * scale,1.f),maxDrawableDimension));
            }

            if(layer.superlayer != hostLayer){
                [layer removeFromSuperlayer];
                [hostLayer addSublayer:layer];
            }

            // Store reference for resizeNativeLayer.
            if([layer isKindOfClass:[CAMetalLayer class]]){
                metalLayer_ = (CAMetalLayer *)layer;
            }

            [hostLayer setNeedsDisplay];
            [layer setNeedsDisplay];
            [_ptr setNeedsDisplay:YES];
            [CATransaction commit];
        }
    }
    void resizeNativeLayer(const Composition::Rect & newRect, float) override {
        if(metalLayer_ == nil){
            return;
        }
        // Frame/bounds/position tracking is owned by Cocoa via the
        // autoresize mask set in setRootLayer. The only resize-time
        // work that remains is drawableSize (which Cocoa does not
        // touch) and contentsScale (Retina transitions).
        constexpr CGFloat kMaxDrawableDimension = 16384.f;
        CGFloat scale = [NSScreen mainScreen].backingScaleFactor;
        if(scale <= 0.f || !std::isfinite(static_cast<double>(scale))){
            scale = 2.f;
        }
        scale = std::max(scale,static_cast<CGFloat>(2.f));
        const CGFloat maxPointDimension = kMaxDrawableDimension / scale;
        const CGFloat w = std::clamp(
                std::isfinite(newRect.w) ? static_cast<CGFloat>(newRect.w) : 1.f,
                static_cast<CGFloat>(1.f),
                maxPointDimension);
        const CGFloat h = std::clamp(
                std::isfinite(newRect.h) ? static_cast<CGFloat>(newRect.h) : 1.f,
                static_cast<CGFloat>(1.f),
                maxPointDimension);
        const CGSize targetDrawable = CGSizeMake(
                std::clamp(w * scale,static_cast<CGFloat>(1.f),kMaxDrawableDimension),
                std::clamp(h * scale,static_cast<CGFloat>(1.f),kMaxDrawableDimension));
        const CGSize currentDrawable = metalLayer_.drawableSize;
        const CGFloat currentScale = metalLayer_.contentsScale;
        constexpr CGFloat kDrawableEpsilon = 0.5f;
        constexpr CGFloat kScaleEpsilon = 1.f / 1024.f;
        if(std::fabs(currentDrawable.width  - targetDrawable.width)  <= kDrawableEpsilon &&
           std::fabs(currentDrawable.height - targetDrawable.height) <= kDrawableEpsilon &&
           std::fabs(currentScale - scale) <= kScaleEpsilon){
            return;
        }
        NSDictionary *noActions = @{
            @"contents":[NSNull null],
            @"contentsScale":[NSNull null]
        };
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        metalLayer_.actions = noActions;
        metalLayer_.contentsScale = scale;
        metalLayer_.drawableSize = targetDrawable;
        [CATransaction commit];
    }
    void setNeedsDisplay();
    void * getBinding() override;
    Composition::Rect & getRect() override {
        return rect;
    }
    explicit CocoaItem(const Composition::Rect & rect,Type _type,SharedHandle<CocoaItem> parent);
    ~CocoaItem();
};

};

};


#endif
