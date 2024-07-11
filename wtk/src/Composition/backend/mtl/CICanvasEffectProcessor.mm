#import <CoreImage/CoreImage.h>

#include "../RenderTarget.h"

namespace OmegaWTK::Composition{


    class CICanvasEffectProcessor : public BackendCanvasEffectProcessor {
    public:
        explicit CICanvasEffectProcessor(SharedHandle<OmegaGTE::GEFence> & fence):BackendCanvasEffectProcessor(fence){

        }
        void applyEffects(SharedHandle<OmegaGTE::GETexture> & dest,SharedHandle<OmegaGTE::GETextureRenderTarget> & textureTarget,OmegaCommon::Vector<std::pair<CanvasEffect::Type,void *>> & effects) override {
            CIContext *context = [CIContext contextWithMTLCommandQueue:(__bridge id<MTLCommandQueue>)textureTarget->nativeCommandQueue()];
            auto src = textureTarget->underlyingTexture();
            auto cb = textureTarget->commandBuffer();
            CIImage *image = [CIImage imageWithMTLTexture:(__bridge id<MTLTexture>)src->native() options:@{}];
            for(auto & e : effects){
                switch (e.first) {
                    case CanvasEffect::DirectionalBlur : {
                        auto * params = (CanvasEffect::DirectionalBlurParams *)e.second;
                        image = [image imageByApplyingFilter:@"CIMotionBlur" withInputParameters:@{@"inputImage":image,@"inputRadius":[NSNumber numberWithFloat:params->radius],@"inputAngle":[NSNumber numberWithFloat:params->angle]}];
                        break;
                    }
                    case CanvasEffect::GaussianBlur : {
                        auto * params = (CanvasEffect::GaussianBlurParams *)e.second;
                        image = [image imageByApplyingFilter:@"CIGaussianBlur" withInputParameters:@{@"inputImage":image,@"inputRadius":[NSNumber numberWithFloat:params->radius]}];
                        break;
                    }
                }
            }
            [context render:image toMTLTexture:(__bridge id<MTLTexture>)dest->native() commandBuffer:(__bridge id<MTLCommandBuffer>)cb->native() bounds:image.extent colorSpace:CGColorSpaceCreateDeviceRGB()];
            textureTarget->submitCommandBuffer(cb);
            textureTarget->commit();
        }
    };

    SharedHandle<BackendCanvasEffectProcessor>
    BackendCanvasEffectProcessor::Create(SharedHandle<OmegaGTE::GEFence> &fence) {
        return SharedHandle<BackendCanvasEffectProcessor>(new CICanvasEffectProcessor(fence));
    }


}