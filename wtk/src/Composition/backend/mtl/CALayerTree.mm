#define TARGET_METAL 1

#include "../RenderTarget.h"
#import "CALayerTree.h"

#import <QuartzCore/QuartzCore.h>
#import <Foundation/Foundation.h>

#include "NativePrivate/macos/CocoaUtils.h"
#include "NativePrivate/macos/CocoaItem.h"
#import <Metal/Metal.h>
#include <algorithm>
#include <cmath>





namespace OmegaWTK::Composition {

    static inline void runOnMainThreadSync(dispatch_block_t block){
        if([NSThread isMainThread]){
            block();
        }
        else {
            dispatch_sync(dispatch_get_main_queue(), block);
        }
    }

    void stopMTLCapture(){
        [[MTLCaptureManager sharedCaptureManager] stopCapture];
    }

    namespace {
        constexpr CGFloat kMaxDrawableDimension = 16384.f;

        static inline CGFloat safeScale(){
            CGFloat scale = [NSScreen mainScreen].backingScaleFactor;
            if(scale <= 0.f || !std::isfinite(static_cast<double>(scale))){
                scale = 2.f;
            }
            return std::max(scale,static_cast<CGFloat>(2.f));
        }

        static inline Core::Rect sanitizeVisualRect(const Core::Rect & candidate){
            const CGFloat scale = safeScale();
            const float maxPointDimension = static_cast<float>(kMaxDrawableDimension / scale);
            Core::Rect sanitized = candidate;
            if(!std::isfinite(sanitized.pos.x)){
                sanitized.pos.x = 0.f;
            }
            if(!std::isfinite(sanitized.pos.y)){
                sanitized.pos.y = 0.f;
            }
            if(!std::isfinite(sanitized.w) || sanitized.w <= 0.f){
                sanitized.w = 1.f;
            }
            if(!std::isfinite(sanitized.h) || sanitized.h <= 0.f){
                sanitized.h = 1.f;
            }
            sanitized.w = std::clamp(sanitized.w,1.f,maxPointDimension);
            sanitized.h = std::clamp(sanitized.h,1.f,maxPointDimension);
            return sanitized;
        }
    }


// OmegaGTE::NativeRenderTargetDescriptor * makeDescForViewRenderTarget(
//                                         ViewRenderTarget *renderTarget){

//     auto cocoaView = (Native::Cocoa::CocoaItem *)renderTarget->getNativePtr();
//     auto *desc = new OmegaGTE::NativeRenderTargetDescriptor;
//     OmegaWTKCocoaView *view = (__bridge OmegaWTKCocoaView *)cocoaView->getBinding();
//     CAMetalLayer *metalLayer = [CAMetalLayer layer];
//     metalLayer.frame = view.frame;
//     metalLayer.contentsScale = [NSScreen mainScreen].backingScaleFactor;
//     view.layer = metalLayer;
//     desc->metalLayer = metalLayer;
//     return desc;
// };

SharedHandle<BackendVisualTree> BackendVisualTree::Create(SharedHandle<ViewRenderTarget> &view) {
    return (SharedHandle<BackendVisualTree>)new MTLCALayerTree(view);
}

 MTLCALayerTree::MTLCALayerTree(SharedHandle<ViewRenderTarget> & renderTarget):
         view(std::dynamic_pointer_cast<Native::Cocoa::CocoaItem>(renderTarget->getNativePtr()))
 {

 };


 Core::SharedPtr<BackendVisualTree::Visual> MTLCALayerTree::makeRootVisual(
                                                             Core::Rect &rect,
                                                             Core::Position & pos,
                                                             ViewPresentTarget & outPresentTarget){
     auto saneRect = sanitizeVisualRect(rect);
     auto sanePos = saneRect.pos;

     CAMetalLayer *layer = [CAMetalLayer layer];
     layer.opaque = NO;
     layer.autoresizingMask = kCALayerNotSizable;
     layer.layoutManager = nil;
     layer.contentsScale = safeScale();
     layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
     layer.framebufferOnly = NO;
     layer.presentsWithTransaction = NO;
     layer.masksToBounds = NO;
     CGColorRef clearColor = CGColorCreateGenericRGB(0.f,0.f,0.f,0.f);
     layer.backgroundColor = clearColor;
     CGColorRelease(clearColor);
     layer.anchorPoint = CGPointMake(0.f,0.f);
     layer.frame = CGRectMake(sanePos.x,sanePos.y,saneRect.w,saneRect.h);
     layer.bounds = CGRectMake(0.f,0.f,saneRect.w,saneRect.h);
     layer.position = CGPointMake(sanePos.x,sanePos.y);
     layer.drawableSize = CGSizeMake(
             std::clamp(saneRect.w * layer.contentsScale,static_cast<CGFloat>(1.f),kMaxDrawableDimension),
             std::clamp(saneRect.h * layer.contentsScale,static_cast<CGFloat>(1.f),kMaxDrawableDimension));

     // Create the native render target — owned by ViewPresentTarget, not the Visual's context.
     OmegaGTE::NativeRenderTargetDescriptor nativeRenderTargetDescriptor {false,layer};
     auto nativeTarget = gte.graphicsEngine->makeNativeRenderTarget(nativeRenderTargetDescriptor);

     CGFloat scale = layer.contentsScale;
     if(scale <= 0.f || !std::isfinite(static_cast<double>(scale))){
         scale = 2.f;
     }
     scale = std::max(scale,static_cast<CGFloat>(2.f));

     outPresentTarget.nativeTarget = nativeTarget;
     outPresentTarget.backingWidth = static_cast<unsigned>(std::clamp(saneRect.w * scale,static_cast<CGFloat>(1.f),kMaxDrawableDimension));
     outPresentTarget.backingHeight = static_cast<unsigned>(std::clamp(saneRect.h * scale,static_cast<CGFloat>(1.f),kMaxDrawableDimension));

     // Root visual's BackendRenderTargetContext is texture-only (nullptr native target).
     SharedHandle<OmegaGTE::GENativeRenderTarget> nullNative = nullptr;
     Core::Rect r {saneRect};
     BackendRenderTargetContext compTarget (r,nullNative,(float)scale);

     return std::shared_ptr<BackendVisualTree::Visual>(new MTLCALayerTree::RootVisual(sanePos,compTarget,layer));
 };

 Core::SharedPtr<BackendVisualTree::Visual> MTLCALayerTree::makeSurfaceVisual(
                                                             Core::Rect &rect,
                                                             Core::Position & pos){
     auto saneRect = sanitizeVisualRect(rect);
     auto sanePos = saneRect.pos;

     CGFloat scale = safeScale();
     SharedHandle<OmegaGTE::GENativeRenderTarget> nullNative = nullptr;
     Core::Rect r {saneRect};
     BackendRenderTargetContext compTarget (r,nullNative,(float)scale);

     return std::shared_ptr<BackendVisualTree::Visual>(new MTLCALayerTree::SurfaceVisual(sanePos,compTarget));
 };

 void MTLCALayerTree::setRootVisual(Core::SharedPtr<Parent::Visual> & visual){
     root = visual;
     auto v = std::dynamic_pointer_cast<RootVisual>(visual);
     if(v != nullptr){
         runOnMainThreadSync(^{
             view->setRootLayer(v->metalLayer);
         });
     }
 };

 void MTLCALayerTree::addVisual(Core::SharedPtr<Parent::Visual> & visual){
     body.push_back(visual);
     auto r = std::dynamic_pointer_cast<Visual>(root);
     auto v = std::dynamic_pointer_cast<Visual>(visual);
     if(v != nullptr){
         ResourceTrace::emit("Bind",
                             "BackendVisual",
                             v->traceResourceId,
                             "MTLCALayerTree::Body",
                             this);
     }
     // Child visual CAMetalLayers are NOT added as sublayers of the root.
     // Their content is composited via the blit pass in compositeAndPresentTarget.
     // Adding them as sublayers would occlude the root's presented drawable
     // with undefined/blank content (orphan CAMetalLayer problem).
 };

};
