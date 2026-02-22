#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#include "omegaWTK/Native/NativeItem.h"

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
    friend class CocoaEventHandler;
    void enable() override;
    void disable() override;
    void addChildNativeItem(NativeItemPtr nativeItem) override;
    void removeChildNativeItem(NativeItemPtr nativeItem) override;

    void setClippedView(NativeItemPtr clippedView) override;
    void toggleHorizontalScrollBar(bool &state) override;
    void toggleVerticalScrollBar(bool &state) override;
public:
    Core::Rect rect;
    typedef enum : OPT_PARAM {
        View,
        ScrollView
    } Type;
private:
    Type type;
public:
    bool isReady;
    void resize(const Core::Rect &newRect) override;
    CALayer *getLayer(){ return [_ptr getCALayer];};
    NSView *getView(){ return _ptr != nil ? (NSView *)_ptr : (NSView *)scrollView; };
    void setRootLayer(CALayer *layer){
        if(_ptr != nil && layer != nil){
            _ptr.wantsLayer = YES;
            NSRect hostBounds = _ptr.bounds;
            if(hostBounds.size.width <= 0.f || hostBounds.size.height <= 0.f){
                hostBounds = _ptr.frame;
            }
            hostBounds.origin = NSMakePoint(0.f,0.f);

            CGFloat scale = [NSScreen mainScreen].backingScaleFactor;
            if(scale <= 0.f){
                scale = 1.f;
            }

            if([layer isKindOfClass:[CAMetalLayer class]]){
                CAMetalLayer *metalLayer = (CAMetalLayer *)layer;
                metalLayer.anchorPoint = CGPointMake(0.f,0.f);
                metalLayer.position = CGPointMake(0.f,0.f);
                metalLayer.bounds = hostBounds;
                metalLayer.frame = hostBounds;
                metalLayer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
                metalLayer.masksToBounds = NO;
                metalLayer.contentsScale = scale;
                metalLayer.drawableSize = CGSizeMake(
                    MAX(hostBounds.size.width * scale,1.f),
                    MAX(hostBounds.size.height * scale,1.f));
                metalLayer.opaque = NO;
                metalLayer.framebufferOnly = NO;
                metalLayer.presentsWithTransaction = NO;
                CGColorRef clearColor = CGColorCreateGenericRGB(0.f,0.f,0.f,0.f);
                metalLayer.backgroundColor = clearColor;
                CGColorRelease(clearColor);
                _ptr.layer = metalLayer;
                _ptr.layerContentsRedrawPolicy = NSViewLayerContentsRedrawDuringViewResize;
                NSLog(@"Root CAMetalLayer attached: frame={%.1f,%.1f,%.1f,%.1f} drawable={%.1f,%.1f} hostSublayers=%lu",
                      metalLayer.frame.origin.x,metalLayer.frame.origin.y,
                      metalLayer.frame.size.width,metalLayer.frame.size.height,
                      metalLayer.drawableSize.width,metalLayer.drawableSize.height,
                      (unsigned long)metalLayer.sublayers.count);
                [metalLayer setNeedsDisplay];
                [_ptr setNeedsDisplay:YES];
                [_ptr layoutSubtreeIfNeeded];
                return;
            }

            if(_ptr.layer == nil){
                _ptr.layer = [CALayer layer];
            }

            CALayer *hostLayer = _ptr.layer;
            hostLayer.anchorPoint = CGPointMake(0.f,0.f);
            hostLayer.position = CGPointMake(0.f,0.f);
            hostLayer.bounds = hostBounds;
            hostLayer.frame = hostBounds;
            hostLayer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
            hostLayer.masksToBounds = NO;
            hostLayer.opaque = NO;
            CGColorRef hostColor = CGColorCreateGenericRGB(0.f,0.f,0.f,0.f);
            hostLayer.backgroundColor = hostColor;
            CGColorRelease(hostColor);
            hostLayer.contentsScale = scale;

            layer.anchorPoint = CGPointMake(0.f,0.f);
            layer.position = CGPointMake(0.f,0.f);
            layer.bounds = hostBounds;
            layer.frame = hostBounds;
            layer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
            layer.masksToBounds = NO;
            layer.contentsScale = scale;
            layer.hidden = NO;

            if(layer.superlayer != hostLayer){
                [layer removeFromSuperlayer];
                [hostLayer addSublayer:layer];
            }

            [hostLayer setNeedsDisplay];
            [_ptr setNeedsDisplay:YES];
            [_ptr layoutSubtreeIfNeeded];
        }
    }
    void setNeedsDisplay();
    void * getBinding() override;
    Core::Rect & getRect() override {
        return rect;
    }
    explicit CocoaItem(const Core::Rect & rect,Type _type,SharedHandle<CocoaItem> parent);
    ~CocoaItem();
};

};

};


#endif
