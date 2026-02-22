#import <Metal/Metal.h>

#include "../RenderTarget.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace OmegaWTK::Composition{

    namespace {
        struct PremultPixel {
            float c0;
            float c1;
            float c2;
            float a;
        };

        static inline float clamp01(float value){
            return std::clamp(value,0.0f,1.0f);
        }

        static inline size_t linearIndex(size_t x,size_t y,size_t width){
            return (y * width) + x;
        }

        static inline PremultPixel samplePremultBilinear(const std::vector<PremultPixel> &src,
                                                         size_t width,
                                                         size_t height,
                                                         float x,
                                                         float y){
            if(width == 0 || height == 0){
                return PremultPixel{0.f,0.f,0.f,0.f};
            }

            x = std::clamp(x,0.0f,static_cast<float>(width - 1));
            y = std::clamp(y,0.0f,static_cast<float>(height - 1));

            size_t x0 = static_cast<size_t>(std::floor(x));
            size_t y0 = static_cast<size_t>(std::floor(y));
            size_t x1 = std::min(x0 + 1,width - 1);
            size_t y1 = std::min(y0 + 1,height - 1);

            float tx = x - static_cast<float>(x0);
            float ty = y - static_cast<float>(y0);

            const auto &p00 = src[linearIndex(x0,y0,width)];
            const auto &p10 = src[linearIndex(x1,y0,width)];
            const auto &p01 = src[linearIndex(x0,y1,width)];
            const auto &p11 = src[linearIndex(x1,y1,width)];

            auto lerp = [](float a,float b,float t){ return a + ((b - a) * t); };

            PremultPixel top{
                lerp(p00.c0,p10.c0,tx),
                lerp(p00.c1,p10.c1,tx),
                lerp(p00.c2,p10.c2,tx),
                lerp(p00.a,p10.a,tx)
            };
            PremultPixel bottom{
                lerp(p01.c0,p11.c0,tx),
                lerp(p01.c1,p11.c1,tx),
                lerp(p01.c2,p11.c2,tx),
                lerp(p01.a,p11.a,tx)
            };

            return PremultPixel{
                lerp(top.c0,bottom.c0,ty),
                lerp(top.c1,bottom.c1,ty),
                lerp(top.c2,bottom.c2,ty),
                lerp(top.a,bottom.a,ty)
            };
        }

        static std::vector<float> makeGaussianKernel(float radius){
            if(radius <= 0.0f){
                return {1.0f};
            }

            int kernelRadius = std::max(1,static_cast<int>(std::ceil(radius * 2.0f)));
            float sigma = std::max(radius * 0.5f,0.5f);
            const int size = (kernelRadius * 2) + 1;
            std::vector<float> kernel(static_cast<size_t>(size),0.0f);

            float sum = 0.0f;
            for(int i = -kernelRadius; i <= kernelRadius; i++){
                const float x = static_cast<float>(i);
                const float value = std::exp(-(x * x) / (2.0f * sigma * sigma));
                kernel[static_cast<size_t>(i + kernelRadius)] = value;
                sum += value;
            }

            if(sum <= 0.0f){
                return {1.0f};
            }

            for(auto &value : kernel){
                value /= sum;
            }
            return kernel;
        }

        static void applyGaussianBlur(const std::vector<PremultPixel> &src,
                                      std::vector<PremultPixel> &dst,
                                      size_t width,
                                      size_t height,
                                      float radius){
            if(src.empty()){
                dst.clear();
                return;
            }

            if(radius <= 0.0f){
                dst = src;
                return;
            }

            const auto kernel = makeGaussianKernel(radius);
            const int kernelRadius = static_cast<int>((kernel.size() - 1) / 2);

            std::vector<PremultPixel> horizontal(src.size(),PremultPixel{0.f,0.f,0.f,0.f});

            for(size_t y = 0; y < height; y++){
                for(size_t x = 0; x < width; x++){
                    float c0 = 0.f,c1 = 0.f,c2 = 0.f,a = 0.f;
                    for(int k = -kernelRadius; k <= kernelRadius; k++){
                        const int xSample = std::clamp(static_cast<int>(x) + k,0,static_cast<int>(width) - 1);
                        const float weight = kernel[static_cast<size_t>(k + kernelRadius)];
                        const auto &sample = src[linearIndex(static_cast<size_t>(xSample),y,width)];
                        c0 += sample.c0 * weight;
                        c1 += sample.c1 * weight;
                        c2 += sample.c2 * weight;
                        a += sample.a * weight;
                    }
                    horizontal[linearIndex(x,y,width)] = PremultPixel{c0,c1,c2,a};
                }
            }

            dst.resize(src.size());
            for(size_t y = 0; y < height; y++){
                for(size_t x = 0; x < width; x++){
                    float c0 = 0.f,c1 = 0.f,c2 = 0.f,a = 0.f;
                    for(int k = -kernelRadius; k <= kernelRadius; k++){
                        const int ySample = std::clamp(static_cast<int>(y) + k,0,static_cast<int>(height) - 1);
                        const float weight = kernel[static_cast<size_t>(k + kernelRadius)];
                        const auto &sample = horizontal[linearIndex(x,static_cast<size_t>(ySample),width)];
                        c0 += sample.c0 * weight;
                        c1 += sample.c1 * weight;
                        c2 += sample.c2 * weight;
                        a += sample.a * weight;
                    }
                    dst[linearIndex(x,y,width)] = PremultPixel{c0,c1,c2,a};
                }
            }
        }

        static void applyDirectionalBlur(const std::vector<PremultPixel> &src,
                                         std::vector<PremultPixel> &dst,
                                         size_t width,
                                         size_t height,
                                         float radius,
                                         float angle){
            if(src.empty()){
                dst.clear();
                return;
            }

            if(radius <= 0.0f){
                dst = src;
                return;
            }

            const int samplesPerSide = std::max(1,static_cast<int>(std::ceil(radius * 2.0f)));
            const float dirX = std::cos(angle);
            const float dirY = std::sin(angle);

            dst.resize(src.size());
            for(size_t y = 0; y < height; y++){
                for(size_t x = 0; x < width; x++){
                    float c0 = 0.f,c1 = 0.f,c2 = 0.f,a = 0.f;
                    float weightSum = 0.f;
                    for(int sample = -samplesPerSide; sample <= samplesPerSide; sample++){
                        const float t = (radius * static_cast<float>(sample)) / static_cast<float>(samplesPerSide);
                        const float sx = static_cast<float>(x) + (dirX * t);
                        const float sy = static_cast<float>(y) + (dirY * t);
                        const auto sp = samplePremultBilinear(src,width,height,sx,sy);
                        c0 += sp.c0;
                        c1 += sp.c1;
                        c2 += sp.c2;
                        a += sp.a;
                        weightSum += 1.f;
                    }
                    if(weightSum > 0.f){
                        c0 /= weightSum;
                        c1 /= weightSum;
                        c2 /= weightSum;
                        a /= weightSum;
                    }
                    dst[linearIndex(x,y,width)] = PremultPixel{c0,c1,c2,a};
                }
            }
        }
    }

    class CICanvasEffectProcessor : public BackendCanvasEffectProcessor {
    public:
        explicit CICanvasEffectProcessor(SharedHandle<OmegaGTE::GEFence> & fence):BackendCanvasEffectProcessor(fence){

        }
        void applyEffects(SharedHandle<OmegaGTE::GETexture> & dest,
                          SharedHandle<OmegaGTE::GETextureRenderTarget> & textureTarget,
                          OmegaCommon::Vector<CanvasEffect> & effects) override {
            auto src = textureTarget->underlyingTexture();
            if(src == nullptr || dest == nullptr){
                return;
            }

            id<MTLTexture> srcTexture = (__bridge id<MTLTexture>)src->native();
            if(srcTexture == nil || srcTexture.width == 0 || srcTexture.height == 0){
                return;
            }

            const size_t width = static_cast<size_t>(srcTexture.width);
            const size_t height = static_cast<size_t>(srcTexture.height);
            const size_t bytesPerRow = width * 4;
            const size_t totalBytes = bytesPerRow * height;

            std::vector<uint8_t> srcBytes(totalBytes,0);
            src->getBytes(srcBytes.data(),bytesPerRow);

            std::vector<PremultPixel> current(width * height,PremultPixel{0.f,0.f,0.f,0.f});
            for(size_t y = 0; y < height; y++){
                for(size_t x = 0; x < width; x++){
                    const size_t byteOffset = (y * bytesPerRow) + (x * 4);
                    const float c0 = static_cast<float>(srcBytes[byteOffset]) / 255.0f;
                    const float c1 = static_cast<float>(srcBytes[byteOffset + 1]) / 255.0f;
                    const float c2 = static_cast<float>(srcBytes[byteOffset + 2]) / 255.0f;
                    const float a = static_cast<float>(srcBytes[byteOffset + 3]) / 255.0f;
                    current[linearIndex(x,y,width)] = PremultPixel{
                        c0 * a,
                        c1 * a,
                        c2 * a,
                        a
                    };
                }
            }

            std::vector<PremultPixel> next;
            for(auto &effect : effects){
                switch(effect.type){
                    case CanvasEffect::DirectionalBlur:
                        applyDirectionalBlur(current,next,width,height,
                                             std::max(0.0f,effect.directionalBlur.radius),
                                             effect.directionalBlur.angle);
                        break;
                    case CanvasEffect::GaussianBlur:
                    default:
                        applyGaussianBlur(current,next,width,height,
                                          std::max(0.0f,effect.gaussianBlur.radius));
                        break;
                }
                current.swap(next);
            }

            std::vector<uint8_t> outBytes(totalBytes,0);
            for(size_t y = 0; y < height; y++){
                for(size_t x = 0; x < width; x++){
                    const auto &px = current[linearIndex(x,y,width)];
                    const float a = clamp01(px.a);
                    const float invA = a > 0.00001f ? (1.0f / a) : 0.0f;

                    const size_t byteOffset = (y * bytesPerRow) + (x * 4);
                    outBytes[byteOffset] = static_cast<uint8_t>(std::lround(clamp01(px.c0 * invA) * 255.0f));
                    outBytes[byteOffset + 1] = static_cast<uint8_t>(std::lround(clamp01(px.c1 * invA) * 255.0f));
                    outBytes[byteOffset + 2] = static_cast<uint8_t>(std::lround(clamp01(px.c2 * invA) * 255.0f));
                    outBytes[byteOffset + 3] = static_cast<uint8_t>(std::lround(a * 255.0f));
                }
            }

            dest->copyBytes(outBytes.data(),bytesPerRow);

            // Keep the existing fence synchronization contract with the final copy pass.
            auto cb = textureTarget->commandBuffer();
            textureTarget->submitCommandBuffer(cb,fence);
            textureTarget->commit();
        }
    };

    SharedHandle<BackendCanvasEffectProcessor>
    BackendCanvasEffectProcessor::Create(SharedHandle<OmegaGTE::GEFence> &fence) {
        return SharedHandle<BackendCanvasEffectProcessor>(new CICanvasEffectProcessor(fence));
    }


}
