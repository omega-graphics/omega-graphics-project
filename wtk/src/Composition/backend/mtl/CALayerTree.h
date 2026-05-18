#include "../VisualTree.h"
#include "../RenderTarget.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

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
         // Logical->physical pixel scale, sourced from the native window
         // via ViewRenderTarget::getRenderScale().
         float renderScale_ = 1.f;
     public:
         using Parent::body;
         using Parent::root;

         /// Root visual — owns a CAMetalLayer for presentation.
         struct RootVisual : public Parent::Visual {
             CAMetalLayer *metalLayer;
             explicit RootVisual(Composition::Point2D & pos,
                    std::unique_ptr<BackendRenderTargetContext> renderTarget,
                    CAMetalLayer *metalLayer):
                     Parent::Visual(pos,std::move(renderTarget)),
                     metalLayer(metalLayer != nil ?
                                (CAMetalLayer *)CFRetain((__bridge CFTypeRef)metalLayer) :
                                nil){
             };
             void resize(Composition::Rect & newRect) override {
                 // Native layer geometry (CAMetalLayer frame/bounds/drawableSize)
                 // is now updated on the main thread by CocoaItem::resizeNativeLayer.
                 // This method only sizes the GPU render target texture.
                 renderTarget->setRenderTargetSize(newRect);
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
             explicit SurfaceVisual(Composition::Point2D & pos,
                    std::unique_ptr<BackendRenderTargetContext> renderTarget):
                     Parent::Visual(pos,std::move(renderTarget)){
             };
             void resize(Composition::Rect & newRect) override {
                 renderTarget->setRenderTargetSize(newRect);
             }
         };

     public:
         explicit MTLCALayerTree(SharedHandle<ViewRenderTarget> & renderTarget);
         ~MTLCALayerTree() override = default;
         void addVisual(Core::SharedPtr<Parent::Visual> & visual) override;

         /// Tier 3 Phase 3.7: drain the per-frame native-content
         /// carve-outs recorded by `BackendRenderTargetContext::
         /// renderToTarget` and translate each record into CA-level
         /// sublayer ordering against this tree's `view`'s root
         /// `CALayer`. The actual hostId → CALayer lookup table is
         /// owned by `NativeViewHost-Adoption-Plan.md` Phases V2 /
         /// G2 (`CocoaItem` registers each embedded native item with
         /// the tree); until that registry lands, this drain logs
         /// the records and clears them. Called by the compositor
         /// frame worker after the slice loop completes.
         void applyNativeContentCarveouts(BackendRenderTargetContext & ctx) override;
         Core::SharedPtr<Parent::Visual> makeRootVisual(Composition::Rect & rect,
                                                         Composition::Point2D & pos,
                                                         ViewPresentTarget & outPresentTarget) override;
         Core::SharedPtr<Parent::Visual> makeSurfaceVisual(Composition::Rect & rect,
                                                            Composition::Point2D & pos) override;
         void setRootVisual(Core::SharedPtr<Parent::Visual> & visual) override;
     };

    

};

#endif
