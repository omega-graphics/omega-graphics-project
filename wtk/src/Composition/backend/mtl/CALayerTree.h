#include "../VisualTree.h"
#include "../RenderTarget.h"

#ifndef OMEGAWTK_COMPOSITION_MTL_MTLBDCALAYERTREE_H
#define OMEGAWTK_COMPOSITION_MTL_MTLBDCALAYERTREE_H

// #ifdef __OBJC__

#include "NativePrivate/macos/CocoaItem.h"
#import <QuartzCore/QuartzCore.h>


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
         /**
          A small structure that holds CAMetalLayer, CATransformLayer,
          as well a SharedHandle to the original BDCompositionImage
          */
         struct Visual : public Parent::Visual {
             CAMetalLayer *metalLayer;
             CATransformLayer *transformLayer;
             bool attachTransformLayer;
             explicit Visual(Core::Position & pos,
                    BackendRenderTargetContext &renderTarget,
                    CAMetalLayer *metalLayer,
                    CATransformLayer *transformLayer,
                    bool attachTransformLayer):
                     Parent::Visual(pos,renderTarget),
                     metalLayer(metalLayer != nil ?
                                (CAMetalLayer *)CFRetain((__bridge CFTypeRef)metalLayer) :
                                nil),
                     transformLayer(transformLayer != nil ?
                                    (CATransformLayer *)CFRetain((__bridge CFTypeRef)transformLayer) :
                                    nil),
                     attachTransformLayer(attachTransformLayer){

             };
             void resize(Core::Rect & newRect) override {
                 [metalLayer setFrame:CGRectMake(newRect.pos.x,newRect.pos.y,newRect.w,newRect.h)];
                 CGFloat scale = metalLayer.contentsScale;
                 metalLayer.drawableSize = CGSizeMake(newRect.w * scale,newRect.h * scale);
             }
             void updateShadowEffect(LayerEffect::DropShadowParams & params) override {
                 CALayer *targetLayer = attachTransformLayer && transformLayer != nil ?
                                        (CALayer *)transformLayer :
                                        (CALayer *)metalLayer;
                 targetLayer.masksToBounds = NO;
                 targetLayer.shadowOpacity = params.opacity;
                 targetLayer.shadowRadius = params.radius;
                 targetLayer.shadowOffset = CGSizeMake(params.x_offset,params.y_offset);
                 auto color = CGColorCreateGenericRGB(params.color.r,params.color.g,params.color.b,params.color.a);
                 targetLayer.shadowColor = color;
                 CGColorRelease(color);
             }
             void updateTransformEffect(LayerEffect::TransformationParams &params) override {
                 CATransformLayer *tLayer = transformLayer;
                 if(!attachTransformLayer){
                     CALayer *superLayer = metalLayer.superlayer;
                     CGPoint pos = metalLayer.position;
                     auto newTransformLayer = [CATransformLayer layer];
                     if(transformLayer != nil){
                         CFRelease((__bridge CFTypeRef)transformLayer);
                     }
                     transformLayer = (CATransformLayer *)CFRetain((__bridge CFTypeRef)newTransformLayer);
                     tLayer = newTransformLayer;
                     tLayer.anchorPoint = CGPointMake(0,0);
                     tLayer.position = pos;
                     tLayer.frame = metalLayer.frame;
                     [superLayer replaceSublayer:metalLayer with:transformLayer];
                     [transformLayer addSublayer:metalLayer];
                     metalLayer.position = CGPointMake(0,0);
                     attachTransformLayer = true;
                 }
                 auto first = CATransform3DMakeTranslation(params.translate.x,params.translate.y,params.translate.z);
                 auto second = CATransform3DConcat(first, CATransform3DMakeRotation(params.rotate.pitch,0.f,0.f,1.f));
                 second = CATransform3DConcat(second, CATransform3DMakeRotation(params.rotate.yaw,0.f,1.f,0.f));
                 second = CATransform3DConcat(second, CATransform3DMakeRotation(params.rotate.roll,1.f,0.f,0.f));
                 auto third = CATransform3DConcat(second, CATransform3DMakeScale(params.scale.x,params.scale.y,params.scale.z));
                 tLayer.transform = third;
             }

             ~Visual() override {
                 if(transformLayer != nil){
                     CFRelease((__bridge CFTypeRef)transformLayer);
                     transformLayer = nil;
                 }
                 if(metalLayer != nil){
                     CFRelease((__bridge CFTypeRef)metalLayer);
                     metalLayer = nil;
                 }
             };
         };
     public:
         explicit MTLCALayerTree(SharedHandle<ViewRenderTarget> & renderTarget);
         ~MTLCALayerTree() override = default;
         void addVisual(Core::SharedPtr<Parent::Visual> & visual) override;
         Core::SharedPtr<Parent::Visual> makeVisual(Core::Rect & rect,
                                                    Core::Position & pos) override;
         void setRootVisual(Core::SharedPtr<Parent::Visual> & visual) override;
     };

    

};

#endif
