#include "../VisualTree.h"
#include "../RenderTarget.h"
#include <algorithm>
#include <cmath>

#ifndef OMEGAWTK_COMPOSITION_MTL_MTLBDCALAYERTREE_H
#define OMEGAWTK_COMPOSITION_MTL_MTLBDCALAYERTREE_H

// #ifdef __OBJC__

#include "NativePrivate/macos/CocoaItem.h"
#import <QuartzCore/QuartzCore.h>
#import <dispatch/dispatch.h>


// #else 

// struct CALayer;
// struct CAMetalLayer;
// struct CATransformLayer;

// #endif

namespace OmegaWTK::Composition {
    // /**
    //  Metal Backend Impl of the BDCompositionVisualTree using CALayers
    //  */
     class MTLCALayerTree : public BackendVisualTree {
         typedef BackendVisualTree Parent;
         SharedHandle<Native::Cocoa::CocoaItem> view;
     public:
         using Parent::body;
         using Parent::root;

         /// Root visual — owns a CAMetalLayer for presentation.
         struct RootVisual : public Parent::Visual {
             CAMetalLayer *metalLayer;
             explicit RootVisual(Core::Position & pos,
                    BackendRenderTargetContext &renderTarget,
                    CAMetalLayer *metalLayer):
                     Parent::Visual(pos,renderTarget),
                     metalLayer(metalLayer != nil ?
                                (CAMetalLayer *)CFRetain((__bridge CFTypeRef)metalLayer) :
                                nil){
             };
             void resize(Core::Rect & newRect) override {
                 if(metalLayer == nil){
                     return;
                 }

                 CGFloat scale = metalLayer.contentsScale;
                 if(scale <= 0.f || !std::isfinite(static_cast<double>(scale))){
                     scale = 2.f;
                 }
                 scale = std::max(scale,static_cast<CGFloat>(2.f));
                 const CGFloat maxDrawableDimension = 16384.f;
                 const CGFloat maxPointDimension = maxDrawableDimension / scale;
                 const CGFloat x = std::isfinite(newRect.pos.x) ? static_cast<CGFloat>(newRect.pos.x) : 0.f;
                 const CGFloat y = std::isfinite(newRect.pos.y) ? static_cast<CGFloat>(newRect.pos.y) : 0.f;
                 const CGFloat w = std::clamp(
                         std::isfinite(newRect.w) ? static_cast<CGFloat>(newRect.w) : 1.f,
                         static_cast<CGFloat>(1.f),
                         maxPointDimension);
                 const CGFloat h = std::clamp(
                         std::isfinite(newRect.h) ? static_cast<CGFloat>(newRect.h) : 1.f,
                         static_cast<CGFloat>(1.f),
                         maxPointDimension);

                 auto applyGeometry = ^{
                     NSDictionary *noActions = @{
                         @"bounds":[NSNull null],
                         @"position":[NSNull null],
                         @"frame":[NSNull null],
                         @"contents":[NSNull null],
                         @"transform":[NSNull null]
                     };
                     [CATransaction begin];
                     [CATransaction setDisableActions:YES];
                     CGRect frame = CGRectMake(x,y,w,h);
                     metalLayer.actions = noActions;
                     [metalLayer setFrame:frame];
                     metalLayer.bounds = CGRectMake(0.f,0.f,w,h);
                     metalLayer.position = CGPointMake(x,y);
                     metalLayer.contentsScale = scale;
                     metalLayer.drawableSize = CGSizeMake(
                             std::clamp(w * scale,static_cast<CGFloat>(1.f),maxDrawableDimension),
                             std::clamp(h * scale,static_cast<CGFloat>(1.f),maxDrawableDimension));
                     [CATransaction commit];
                 };
                 if([NSThread isMainThread]){
                     applyGeometry();
                 }
                 else {
                     dispatch_sync(dispatch_get_main_queue(),applyGeometry);
                 }
             }

             ~RootVisual() override {
                 if(metalLayer != nil){
                     CFRelease((__bridge CFTypeRef)metalLayer);
                     metalLayer = nil;
                 }
             };
         };

         /// Surface-only visual — GPU texture, no native layer.
         struct SurfaceVisual : public Parent::Visual {
             explicit SurfaceVisual(Core::Position & pos,
                    BackendRenderTargetContext & renderTarget):
                     Parent::Visual(pos,renderTarget){
             };
             void resize(Core::Rect & newRect) override {
                 renderTarget.setRenderTargetSize(newRect);
             }
         };

     public:
         explicit MTLCALayerTree(SharedHandle<ViewRenderTarget> & renderTarget);
         ~MTLCALayerTree() override = default;
         void addVisual(Core::SharedPtr<Parent::Visual> & visual) override;
         Core::SharedPtr<Parent::Visual> makeRootVisual(Core::Rect & rect,
                                                         Core::Position & pos,
                                                         ViewPresentTarget & outPresentTarget) override;
         Core::SharedPtr<Parent::Visual> makeSurfaceVisual(Core::Rect & rect,
                                                            Core::Position & pos) override;
         void setRootVisual(Core::SharedPtr<Parent::Visual> & visual) override;
     };

    

};

#endif
