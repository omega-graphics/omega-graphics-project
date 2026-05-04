#include "Effect.h"

#include "Pipeline.h"
#include "ResourceFactory.h"

#include <algorithm>

namespace OmegaWTK::Composition {

    namespace {
        inline PipelineRegistry & pipelineRegistry(){
            return BackendResourceFactory::instance().pipelines();
        }
    }

    /// Unified cross-platform effect processor using OmegaSL compute shaders.
    /// All pipeline state objects (gaussian/directional blur), the shared
    /// `bufferWriter`, and any other GPU resources are obtained from the
    /// process-wide `PipelineRegistry`. The processor itself is stateless;
    /// the per-layer scratch's fence is supplied per call.
    class GPUCanvasEffectProcessor : public BackendCanvasEffectProcessor {
    public:
        GPUCanvasEffectProcessor() = default;

        void applyEffects(SharedHandle<OmegaGTE::GETexture> & dest,
                          SharedHandle<OmegaGTE::GETextureRenderTarget> & textureTarget,
                          OmegaCommon::Vector<CanvasEffect> & effects,
                          unsigned width,
                          unsigned height,
                          SharedHandle<OmegaGTE::GEFence> & fence) override {
            if(effects.empty()){
                return;
            }
            auto src = textureTarget->underlyingTexture();
            if(src == nullptr || dest == nullptr){
                return;
            }
            if(width == 0 || height == 0){
                return;
            }
            auto & pipelines = pipelineRegistry();
            auto bufferWriter = pipelines.bufferWriter();
            auto gaussianBlurHPipelineState = pipelines.gaussianBlurH();
            auto gaussianBlurVPipelineState = pipelines.gaussianBlurV();
            auto directionalBlurPipelineState = pipelines.directionalBlur();
            if(bufferWriter == nullptr){
                return;
            }

            for(auto & effect : effects){
                switch(effect.type){
                    case CanvasEffect::Type::GaussianBlur: {
                        auto blurH = gaussianBlurHPipelineState;
                        auto blurV = gaussianBlurVPipelineState;
                        if(blurH == nullptr || blurV == nullptr){ break; }
                        float radius = std::max(0.f, effect.gaussianBlur.radius);
                        if(radius <= 0.f){ break; }

                        // BlurParams: float radius, uint texWidth, uint texHeight, float angle
                        auto structSize = OmegaGTE::omegaSLStructStride({OMEGASL_FLOAT,OMEGASL_UINT,OMEGASL_UINT,OMEGASL_FLOAT});
                        OmegaGTE::BufferDescriptor bd {OmegaGTE::BufferDescriptor::Upload,structSize,structSize};
                        auto pb = gte.graphicsEngine->makeBuffer(bd);
                        if(pb == nullptr){ break; }
                        bufferWriter->setOutputBuffer(pb);
                        float angle = 0.f;
                        bufferWriter->structBegin();
                        bufferWriter->writeFloat(radius);
                        bufferWriter->writeUint(width);
                        bufferWriter->writeUint(height);
                        bufferWriter->writeFloat(angle);
                        bufferWriter->structEnd();
                        bufferWriter->sendToBuffer();
                        bufferWriter->flush();

                        unsigned gx = (width + 7) / 8;
                        unsigned gy = (height + 7) / 8;

                        // H pass: src → dest
                        {
                            auto cb = textureTarget->commandBuffer();
                            cb->startComputePass(blurH);
                            cb->bindResourceAtComputeShader(pb, 5);
                            cb->bindResourceAtComputeShader(src, 3);
                            cb->bindResourceAtComputeShader(dest, 4);
                            cb->dispatchThreadgroups(gx, gy, 1);
                            cb->endComputePass();
                            textureTarget->submitCommandBuffer(cb);
                        }
                        // V pass: dest → src (ping-pong)
                        {
                            auto cb = textureTarget->commandBuffer();
                            cb->startComputePass(blurV);
                            cb->bindResourceAtComputeShader(pb, 5);
                            cb->bindResourceAtComputeShader(dest, 3);
                            cb->bindResourceAtComputeShader(src, 4);
                            cb->dispatchThreadgroups(gx, gy, 1);
                            cb->endComputePass();
                            textureTarget->submitCommandBuffer(cb);
                        }
                        // After H→dest, V→src ping-pong, result is back in src
                        // (the offscreen target's underlying texture), which
                        // commit() blits to the native drawable.
                        break;
                    }
                    case CanvasEffect::Type::DirectionalBlur: {
                        auto dirPipe = directionalBlurPipelineState;
                        if(dirPipe == nullptr){ break; }
                        float radius = std::max(0.f, effect.directionalBlur.radius);
                        if(radius <= 0.f){ break; }

                        float dirAngle = effect.directionalBlur.angle;

                        auto structSize = OmegaGTE::omegaSLStructStride({OMEGASL_FLOAT,OMEGASL_UINT,OMEGASL_UINT,OMEGASL_FLOAT});
                        OmegaGTE::BufferDescriptor bd {OmegaGTE::BufferDescriptor::Upload,structSize,structSize};
                        auto pb = gte.graphicsEngine->makeBuffer(bd);
                        if(pb == nullptr){ break; }
                        bufferWriter->setOutputBuffer(pb);
                        bufferWriter->structBegin();
                        bufferWriter->writeFloat(radius);
                        bufferWriter->writeUint(width);
                        bufferWriter->writeUint(height);
                        bufferWriter->writeFloat(dirAngle);
                        bufferWriter->structEnd();
                        bufferWriter->sendToBuffer();
                        bufferWriter->flush();

                        unsigned gx = (width + 7) / 8;
                        unsigned gy = (height + 7) / 8;

                        // Pass 1: directional blur src→dest
                        {
                            auto cb = textureTarget->commandBuffer();
                            cb->startComputePass(dirPipe);
                            cb->bindResourceAtComputeShader(pb, 5);
                            cb->bindResourceAtComputeShader(src, 3);
                            cb->bindResourceAtComputeShader(dest, 4);
                            cb->dispatchThreadgroups(gx, gy, 1);
                            cb->endComputePass();
                            textureTarget->submitCommandBuffer(cb);
                        }
                        // Pass 2: copy dest→src using H blur with zero radius (identity)
                        {
                            auto pb2 = gte.graphicsEngine->makeBuffer(bd);
                            if(pb2 != nullptr){
                                float zeroRadius = 0.f;
                                float zeroAngle = 0.f;
                                bufferWriter->setOutputBuffer(pb2);
                                bufferWriter->structBegin();
                                bufferWriter->writeFloat(zeroRadius);
                                bufferWriter->writeUint(width);
                                bufferWriter->writeUint(height);
                                bufferWriter->writeFloat(zeroAngle);
                                bufferWriter->structEnd();
                                bufferWriter->sendToBuffer();
                                bufferWriter->flush();

                                auto blurH = gaussianBlurHPipelineState;
                                if(blurH != nullptr){
                                    auto cb2 = textureTarget->commandBuffer();
                                    cb2->startComputePass(blurH);
                                    cb2->bindResourceAtComputeShader(pb2, 5);
                                    cb2->bindResourceAtComputeShader(dest, 3);
                                    cb2->bindResourceAtComputeShader(src, 4);
                                    cb2->dispatchThreadgroups(gx, gy, 1);
                                    cb2->endComputePass();
                                    textureTarget->submitCommandBuffer(cb2);
                                }
                            }
                        }
                        break;
                    }
                }
            }

            // Fence synchronization for the composite pass. The caller's
            // fence (the per-layer scratch's fence) is signaled so the
            // composite quad on the swap-chain CB waits for blur completion.
            auto cb = textureTarget->commandBuffer();
            textureTarget->submitCommandBuffer(cb, fence);
            textureTarget->commit();
        }
    };

    SharedHandle<BackendCanvasEffectProcessor>
    BackendCanvasEffectProcessor::Create(){
        return SharedHandle<BackendCanvasEffectProcessor>(new GPUCanvasEffectProcessor());
    }

}
