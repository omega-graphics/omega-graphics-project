// §2.14 Pass 1 (macOS) — Native side. Constructs the per-window
// CAMetalLayer, sanitizes its initial geometry, attaches it to the
// host NSView via CocoaItem::setRootLayer, and hands the typed
// MTLVisual to the abstract VisualTree surface. The GENativeRender
// Target / BackendRenderTargetContext lives in the Composition layer
// (`MTLVisualBinder.mm`) — Native does not see GTE types.
//
// COMPILE-UNVERIFIED off-platform. The Linux migration on the same
// commit is build- and run-verified; this file mirrors that pattern
// closely (same SharedHandle ownership, same `setOnResize` install
// site in the binder) so it should land green on the first macOS
// build. Drift between the platforms would be the first thing to
// audit if it doesn't.

#import "NativePrivate/macos/MTLVisualTree.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <cmath>

namespace OmegaWTK::Native::Cocoa {

    namespace {

        // CAMetalLayer.drawableSize ceiling — Metal's
        // MTKView/CAMetalLayer documents 16384 as the supported max on
        // recent macOS. Carrying the same clamp pre-§2.14 used
        // (MTLCALayerTree::sanitizeVisualRect) so initial geometry
        // never exceeds GPU limits.
        constexpr CGFloat kMaxDrawableDimension = 16384.f;

        CGFloat safeBackingScale(){
            CGFloat scale = [NSScreen mainScreen].backingScaleFactor;
            if(scale <= 0.f || !std::isfinite(static_cast<double>(scale))){
                scale = 2.f;
            }
            return std::max(scale, static_cast<CGFloat>(2.f));
        }

        Composition::Rect sanitizeRect(const Composition::Rect & candidate){
            const CGFloat scale = safeBackingScale();
            const float maxPointDimension =
                static_cast<float>(kMaxDrawableDimension / scale);
            Composition::Rect sanitized = candidate;
            if(!std::isfinite(sanitized.pos.x)) sanitized.pos.x = 0.f;
            if(!std::isfinite(sanitized.pos.y)) sanitized.pos.y = 0.f;
            if(!std::isfinite(sanitized.w) || sanitized.w <= 0.f) sanitized.w = 1.f;
            if(!std::isfinite(sanitized.h) || sanitized.h <= 0.f) sanitized.h = 1.f;
            sanitized.w = std::clamp(sanitized.w, 1.f, maxPointDimension);
            sanitized.h = std::clamp(sanitized.h, 1.f, maxPointDimension);
            return sanitized;
        }

        CAMetalLayer * buildMetalLayer(const Composition::Rect & rect, float renderScale){
            CAMetalLayer *layer = [CAMetalLayer layer];
            layer.opaque = NO;
            layer.autoresizingMask = kCALayerNotSizable;
            layer.layoutManager = nil;
            layer.contentsScale = renderScale;
            // BGRA8Unorm: the WTK compositor pipelines + glyph atlas
            // expect this. Set explicitly so the surface format isn't
            // tied to whatever Metal picks by default.
            layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
            layer.framebufferOnly = NO;
            layer.presentsWithTransaction = NO;
            layer.masksToBounds = NO;
            CGColorRef clearColor = CGColorCreateGenericRGB(0.f, 0.f, 0.f, 0.f);
            layer.backgroundColor = clearColor;
            CGColorRelease(clearColor);
            layer.anchorPoint = CGPointMake(0.f, 0.f);
            layer.frame = CGRectMake(rect.pos.x, rect.pos.y, rect.w, rect.h);
            layer.bounds = CGRectMake(0.f, 0.f, rect.w, rect.h);
            layer.position = CGPointMake(rect.pos.x, rect.pos.y);
            layer.drawableSize = CGSizeMake(
                std::clamp(rect.w * layer.contentsScale,
                           static_cast<CGFloat>(1.f), kMaxDrawableDimension),
                std::clamp(rect.h * layer.contentsScale,
                           static_cast<CGFloat>(1.f), kMaxDrawableDimension));
            return layer;
        }

    }

    MTLVisual::MTLVisual(SharedHandle<CocoaItem> item,
                          Composition::Rect rect,
                          CAMetalLayer *layer):
        Visual(rect),
        item_(std::move(item)),
        metalLayer_(layer != nil
                     ? (CAMetalLayer *)CFRetain((__bridge CFTypeRef)layer)
                     : nil) {}

    MTLVisual::~MTLVisual(){
        if(metalLayer_ != nil){
            CFRelease((__bridge CFTypeRef)metalLayer_);
            metalLayer_ = nil;
        }
    }

    MTLVisualTree::MTLVisualTree(SharedHandle<CocoaItem> rootItem,
                                  Composition::Rect rect,
                                  float scale):
        scale_(scale > 0.f && std::isfinite(scale) ? scale : 1.f)
    {
        const Composition::Rect saneRect = sanitizeRect(rect);
        CAMetalLayer *layer = buildMetalLayer(saneRect, scale_);
        rootVisual_ = std::make_shared<MTLVisual>(rootItem, saneRect, layer);
        // Attach the layer to the host NSView. Pre-§2.14 this ran
        // inside `MTLCALayerTree::setRootVisual` via a
        // dispatch_sync(main) trampoline; the AppWindow ctor is on
        // the main thread already, so we can call directly.
        if(rootItem != nullptr && layer != nil){
            rootItem->setRootLayer(layer);
        }
    }

    Native::Visual * MTLVisualTree::rootVisual() const {
        return rootVisual_.get();
    }

}

namespace OmegaWTK::Native {

    NativeVisualTreePtr make_native_visual_tree(NativeItemPtr rootItem,
                                                 const Composition::Rect & rect,
                                                 float scale){
        auto cocoaItem = std::dynamic_pointer_cast<Cocoa::CocoaItem>(rootItem);
        if(cocoaItem == nullptr){
            return nullptr;
        }
        return std::make_shared<Cocoa::MTLVisualTree>(std::move(cocoaItem), rect, scale);
    }

}
