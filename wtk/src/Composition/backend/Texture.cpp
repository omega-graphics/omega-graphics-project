#include "Texture.h"

#include "Pipeline.h"
#include "ResourceFactory.h"
#include "TexturePool.h"
#include "omegaWTK/Core/Core.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <utility>

namespace OmegaWTK::Composition {

    namespace {
        constexpr float kMaxTextureDimension = 16384.f;
#if defined(TARGET_MACOS)
        constexpr float kRenderScaleFloor = 2.f;
#else
        constexpr float kRenderScaleFloor = 1.f;
#endif

        inline float sanitizeRenderScale(float scale){
            if(!std::isfinite(scale) || scale <= 0.f){
                return kRenderScaleFloor;
            }
            return std::clamp(scale,kRenderScaleFloor,kMaxTextureDimension);
        }

        inline float sanitizeCoordinate(float value,float fallback){
            if(!std::isfinite(value)){
                return fallback;
            }
            return value;
        }

        inline Composition::Rect sanitizeRenderRect(const Composition::Rect & candidate,
                                                    const Composition::Rect & fallback,
                                                    float renderScale){
            Composition::Rect sanitizedFallback = fallback;
            sanitizedFallback.pos.x = sanitizeCoordinate(sanitizedFallback.pos.x,0.f);
            sanitizedFallback.pos.y = sanitizeCoordinate(sanitizedFallback.pos.y,0.f);
            if(!std::isfinite(sanitizedFallback.w) || sanitizedFallback.w <= 0.f){
                sanitizedFallback.w = 1.f;
            }
            if(!std::isfinite(sanitizedFallback.h) || sanitizedFallback.h <= 0.f){
                sanitizedFallback.h = 1.f;
            }

            const float scale = sanitizeRenderScale(renderScale);
            const float maxLogicalDimension = std::max(1.f,kMaxTextureDimension / scale);
            sanitizedFallback.w = std::clamp(sanitizedFallback.w,1.f,maxLogicalDimension);
            sanitizedFallback.h = std::clamp(sanitizedFallback.h,1.f,maxLogicalDimension);

            Composition::Rect sanitized = candidate;
            sanitized.pos.x = sanitizeCoordinate(sanitized.pos.x,sanitizedFallback.pos.x);
            sanitized.pos.y = sanitizeCoordinate(sanitized.pos.y,sanitizedFallback.pos.y);

            if(!std::isfinite(sanitized.w) || sanitized.w <= 0.f){
                sanitized.w = sanitizedFallback.w;
            }
            if(!std::isfinite(sanitized.h) || sanitized.h <= 0.f){
                sanitized.h = sanitizedFallback.h;
            }

            sanitized.w = std::clamp(sanitized.w,1.f,maxLogicalDimension);
            sanitized.h = std::clamp(sanitized.h,1.f,maxLogicalDimension);
            return sanitized;
        }

        inline unsigned toBackingDimension(float logicalDimension,float renderScale){
            const float saneScale = sanitizeRenderScale(renderScale);
            float saneLogical = logicalDimension;
            if(!std::isfinite(saneLogical) || saneLogical <= 0.f){
                saneLogical = 1.f;
            }
            const auto scaled = static_cast<long>(std::lround(saneLogical * saneScale));
            const auto clamped = std::clamp<long>(scaled,1L,static_cast<long>(kMaxTextureDimension));
            return static_cast<unsigned>(clamped);
        }

        inline TexturePool * texturePool(){
            return BackendResourceFactory::instance().texturePool();
        }
        inline PipelineRegistry & pipelineRegistry(){
            return BackendResourceFactory::instance().pipelines();
        }
    }

    BackingTextureSet::BackingTextureSet(const Composition::Rect & rect,
                                         float renderScale,
                                         SharedHandle<OmegaGTE::GENativeRenderTarget> nativeTarget):
        nativeTarget_(std::move(nativeTarget)),
        renderTargetSize_(rect),
        renderScale_(sanitizeRenderScale(renderScale))
    {
        renderTargetSize_ = sanitizeRenderRect(rect,
                                               Composition::Rect{Composition::Point2D{0.f,0.f},1.f,1.f},
                                               renderScale_);
        backingWidth_  = toBackingDimension(renderTargetSize_.w, renderScale_);
        backingHeight_ = toBackingDimension(renderTargetSize_.h, renderScale_);
    }

    BackingTextureSet::~BackingTextureSet(){
        TexturePoolKey poolKey {
            backingWidth_,
            backingHeight_,
            OmegaGTE::TexturePixelFormat::BGRA8Unorm,
            OmegaGTE::GETexture::RenderTarget
        };
        if(texturePool() && (targetTexture_ || effectTexture_)){
#ifdef _WIN32
            // The Win32 backend must drain GPU work owned by either render
            // target before letting the textures rejoin the pool — otherwise
            // a sibling context can pick them up while a frame is still in
            // flight on this one.
            if(preEffectTarget_ != nullptr){
                preEffectTarget_->waitForGPU();
            }
            if(nativeTarget_ != nullptr){
                nativeTarget_->waitForGPU();
            }
#endif
            if(targetTexture_){
                texturePool()->release(std::move(targetTexture_), poolKey);
            }
            if(effectTexture_){
                texturePool()->release(std::move(effectTexture_), poolKey);
            }
        }
        preEffectTarget_.reset();
        effectTarget_.reset();
        tessellationEngineContext_.reset();
    }

    void BackingTextureSet::releaseTexturesToPool(){
        TexturePoolKey poolKey {
            backingWidth_,
            backingHeight_,
            OmegaGTE::TexturePixelFormat::BGRA8Unorm,
            OmegaGTE::GETexture::RenderTarget
        };
        if(texturePool() && (targetTexture_ || effectTexture_)){
#ifdef _WIN32
            if(nativeTarget_ != nullptr){
                nativeTarget_->waitForGPU();
            }
#endif
            if(targetTexture_){
                texturePool()->release(std::move(targetTexture_), poolKey);
            }
            if(effectTexture_){
                texturePool()->release(std::move(effectTexture_), poolKey);
            }
        }
        targetTexture_.reset();
        effectTexture_.reset();
    }

    void BackingTextureSet::recomputeBackingDimensions(){
        renderTargetSize_ = sanitizeRenderRect(renderTargetSize_,
                                               Composition::Rect{Composition::Point2D{0.f,0.f},1.f,1.f},
                                               renderScale_);
        backingWidth_  = toBackingDimension(renderTargetSize_.w, renderScale_);
        backingHeight_ = toBackingDimension(renderTargetSize_.h, renderScale_);
    }

    void BackingTextureSet::rebuild(){
        recomputeBackingDimensions();

        TexturePoolKey poolKey {
            backingWidth_,
            backingHeight_,
            OmegaGTE::TexturePixelFormat::BGRA8Unorm,
            OmegaGTE::GETexture::RenderTarget
        };

        releaseTexturesToPool();

        // Texture and render-target allocation is thread-safe on Metal and
        // D3D12. Previously this block dispatched synchronously to the main
        // thread, which deadlocked the compositor during live resize because
        // the main thread was busy in NSEventTrackingRunLoopMode.
        if(texturePool()){
            targetTexture_ = texturePool()->acquire(poolKey);
            effectTexture_ = texturePool()->acquire(poolKey);
        }
        else {
            OmegaGTE::TextureDescriptor textureDescriptor {};
            textureDescriptor.usage = OmegaGTE::GETexture::RenderTarget;
            textureDescriptor.storage_opts = OmegaGTE::Shared;
            textureDescriptor.width = backingWidth_;
            textureDescriptor.height = backingHeight_;
            textureDescriptor.type = OmegaGTE::GETexture::Texture2D;
            textureDescriptor.pixelFormat = OmegaGTE::TexturePixelFormat::BGRA8Unorm;
            targetTexture_ = gte.graphicsEngine->makeTexture(textureDescriptor);
            effectTexture_ = gte.graphicsEngine->makeTexture(textureDescriptor);
        }

        if(targetTexture_ == nullptr || effectTexture_ == nullptr){
#ifdef OMEGAWTK_TRACE_RENDER
            std::cout << "Failed to allocate backing textures." << std::endl;
#endif
            preEffectTarget_.reset();
            effectTarget_.reset();
            tessellationEngineContext_.reset();
            return;
        }
        preEffectTarget_ = gte.graphicsEngine->makeTextureRenderTarget({true,targetTexture_});
        effectTarget_    = gte.graphicsEngine->makeTextureRenderTarget({true,effectTexture_});
        if(preEffectTarget_ == nullptr || effectTarget_ == nullptr){
#ifdef OMEGAWTK_TRACE_RENDER
            std::cout << "Failed to allocate Vulkan texture render targets." << std::endl;
#endif
            tessellationEngineContext_.reset();
            return;
        }
        tessellationEngineContext_ = gte.triangulationEngine->createTEContextFromTextureRenderTarget(preEffectTarget_);
        if(tessellationEngineContext_ == nullptr){
#ifdef OMEGAWTK_TRACE_RENDER
            std::cout << "Failed to create tessellation context for backing render target." << std::endl;
#endif
        }
    }

    bool BackingTextureSet::resizeLogical(const Composition::Rect & rect){
        const unsigned oldW = backingWidth_;
        const unsigned oldH = backingHeight_;

        const auto saneRect = sanitizeRenderRect(rect, renderTargetSize_, renderScale_);
        const unsigned newW = toBackingDimension(saneRect.w, renderScale_);
        const unsigned newH = toBackingDimension(saneRect.h, renderScale_);

        renderTargetSize_ = saneRect;
        return (oldW != newW) || (oldH != newH);
    }

    bool BackingTextureSet::applyViewportOverride(float offsetX, float offsetY,
                                                  float width, float height){
        renderTargetSize_.pos.x = 0.f;
        renderTargetSize_.pos.y = 0.f;
        renderTargetSize_.w = width;
        renderTargetSize_.h = height;

        const unsigned neededW = toBackingDimension(offsetX + width,  renderScale_);
        const unsigned neededH = toBackingDimension(offsetY + height, renderScale_);
        if(neededW > backingWidth_ || neededH > backingHeight_){
            // Bumping these here matches the legacy ordering even though
            // rebuild() will recompute them from renderTargetSize_ before
            // allocating textures.
            backingWidth_  = std::max(backingWidth_,  neededW);
            backingHeight_ = std::max(backingHeight_, neededH);
            return true;
        }
        return false;
    }

    void BackingTextureSet::presentBlit(SharedHandle<OmegaGTE::GEFence> & fence){
        if(nativeTarget_ == nullptr || preEffectTarget_ == nullptr){
            return;
        }
        // The effect result is in preEffectTarget's underlying texture. Do a
        // single fenced blit to the native drawable.
        auto tex = preEffectTarget_->underlyingTexture();
        if(tex == nullptr){
            return;
        }
        auto nativeFormat = nativeTarget_->pixelFormat();
        auto finalPipeline = pipelineRegistry().finalCopyForFormat(nativeFormat);
        if(finalPipeline == nullptr){
            return;
        }
        auto cb = nativeTarget_->commandBuffer();
        nativeTarget_->notifyCommandBuffer(cb, fence);

        OmegaGTE::GERenderTarget::RenderPassDesc rpd {};
        rpd.depthStencilAttachment.disabled = true;
        rpd.colorAttachments.push_back(OmegaGTE::GERenderTarget::RenderPassDesc::ColorAttachment(
                OmegaGTE::GERenderTarget::RenderPassDesc::ColorAttachment::ClearColor(0.f,0.f,0.f,0.f),
                OmegaGTE::GERenderTarget::RenderPassDesc::ColorAttachment::LoadAction::Clear));
        cb->startRenderPass(rpd);
        cb->setRenderPipelineState(finalPipeline);
        OmegaGTE::GEViewport vp {};
        vp.x = 0.f; vp.y = 0.f;
        vp.nearDepth = 0.f; vp.farDepth = 1.f;
        vp.width  = static_cast<float>(backingWidth_);
        vp.height = static_cast<float>(backingHeight_);
        OmegaGTE::GEScissorRect sr {0.f, 0.f, static_cast<float>(backingWidth_), static_cast<float>(backingHeight_)};
        cb->setViewports({vp});
        cb->setScissorRects({sr});
        auto quadBuffer = pipelineRegistry().fullscreenQuadBuffer();
        cb->bindResourceAtVertexShader(quadBuffer, 1);
        cb->bindResourceAtFragmentShader(tex, 2);
        cb->drawPolygons(OmegaGTE::GERenderTarget::CommandBuffer::Triangle, 6, 0);
        cb->endRenderPass();
        nativeTarget_->submitCommandBuffer(cb);
        nativeTarget_->commitAndPresent();
    }

    void BackingTextureSet::uploadGradientTexture(bool /*linearOrRadial*/,
                                                  Gradient & gradient,
                                                  OmegaGTE::GRect & rect,
                                                  SharedHandle<OmegaGTE::GETexture> & dest){
        auto bufferWriter = pipelineRegistry().bufferWriter();
        auto linearGradientPipelineState = pipelineRegistry().linearGradient();
        if(nativeTarget_ == nullptr || dest == nullptr || bufferWriter == nullptr || linearGradientPipelineState == nullptr){
            return;
        }
        auto cb = nativeTarget_->commandBuffer();

        size_t structSize = OmegaGTE::omegaSLStructStride({OMEGASL_FLOAT});

        OmegaGTE::BufferDescriptor bufferDescriptor {OmegaGTE::BufferDescriptor::Upload,structSize,structSize,OmegaGTE::Shared};

        auto constBuffer = gte.graphicsEngine->makeBuffer(bufferDescriptor);

//        bufferWriter->setOutputBuffer(constBuffer);
//        bufferWriter->structBegin();
//        bufferWriter->writeFloat(gradient);
//        bufferWriter->structEnd();
//        bufferWriter->sendToBuffer();
//        bufferWriter->flush();

        structSize = OmegaGTE::omegaSLStructStride({OMEGASL_FLOAT,OMEGASL_FLOAT4});

        bufferDescriptor.len = structSize * gradient.stops.size();
        bufferDescriptor.objectStride = structSize;

        auto stopsBuffer = gte.graphicsEngine->makeBuffer(bufferDescriptor);

        bufferWriter->setOutputBuffer(stopsBuffer);

        for(auto & stop : gradient.stops){
            bufferWriter->structBegin();
            bufferWriter->writeFloat(stop.pos);
            auto vec_color = OmegaGTE::makeColor(stop.color.r,stop.color.g,stop.color.b,stop.color.a);
            bufferWriter->writeFloat4(vec_color);
            bufferWriter->structEnd();
            bufferWriter->sendToBuffer();
        }

        bufferWriter->flush();

        cb->startComputePass(linearGradientPipelineState);
        cb->bindResourceAtComputeShader(dest,4);
        cb->bindResourceAtComputeShader(constBuffer,5);
        cb->bindResourceAtComputeShader(stopsBuffer,3);
        cb->dispatchThreads((unsigned)rect.w,(unsigned)rect.h,1);
        cb->endComputePass();
        nativeTarget_->submitCommandBuffer(cb);
    }

}
