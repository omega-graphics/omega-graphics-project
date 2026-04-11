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
             explicit RootVisual(Composition::Point2D & pos,
                    BackendRenderTargetContext &renderTarget,
                    CAMetalLayer *metalLayer):
                     Parent::Visual(pos,renderTarget),
                     metalLayer(metalLayer != nil ?
                                (CAMetalLayer *)CFRetain((__bridge CFTypeRef)metalLayer) :
                                nil){
             };
             void resize(Composition::Rect & newRect) override {
                 // Native layer geometry (CAMetalLayer frame/bounds/drawableSize)
                 // is now updated on the main thread by CocoaItem::resizeNativeLayer.
                 // This method only sizes the GPU render target texture.
                 renderTarget.setRenderTargetSize(newRect);
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
                    BackendRenderTargetContext & renderTarget):
                     Parent::Visual(pos,renderTarget){
             };
             void resize(Composition::Rect & newRect) override {
                 renderTarget.setRenderTargetSize(newRect);
             }
         };

     public:
         explicit MTLCALayerTree(SharedHandle<ViewRenderTarget> & renderTarget);
         ~MTLCALayerTree() override = default;
         void addVisual(Core::SharedPtr<Parent::Visual> & visual) override;
         Core::SharedPtr<Parent::Visual> makeRootVisual(Composition::Rect & rect,
                                                         Composition::Point2D & pos,
                                                         ViewPresentTarget & outPresentTarget) override;
         Core::SharedPtr<Parent::Visual> makeSurfaceVisual(Composition::Rect & rect,
                                                            Composition::Point2D & pos) override;
         void setRootVisual(Core::SharedPtr<Parent::Visual> & visual) override;
     };

    

};

#endif
