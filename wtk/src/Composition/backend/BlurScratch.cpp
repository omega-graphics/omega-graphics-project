#include "BlurScratch.h"

#include "FencePool.h"
#include "ResourceFactory.h"
#include "ResourceTrace.h"
#include "TexturePool.h"

#include <algorithm>

namespace OmegaWTK::Composition {

    namespace {
        inline TexturePool * texturePool(){
            return BackendResourceFactory::instance().texturePool();
        }
        inline FencePool * fencePool(){
            return BackendResourceFactory::instance().fencePool();
        }

        TexturePoolKey makeKey(unsigned w, unsigned h, OmegaGTE::PixelFormat fmt){
            TexturePoolKey key {};
            key.width = w;
            key.height = h;
            key.pixelFormat = fmt;
            key.usage = OmegaGTE::GETexture::RenderTarget;
            return key;
        }
    }

    LayerBlurScratch::LayerBlurScratch() = default;

    LayerBlurScratch::~LayerBlurScratch(){
        release();
    }

    void LayerBlurScratch::release(){
        // Hand textures back to the pool when one is available; otherwise the
        // SharedHandle resets free the GPU resources directly.
        if(texturePool() != nullptr){
            auto key = makeKey(width_, height_, pixelFormat_);
            if(source_){
                texturePool()->release(std::move(source_), key);
            }
            if(pingPong_){
                texturePool()->release(std::move(pingPong_), key);
            }
        }
        source_.reset();
        pingPong_.reset();
        sourceTarget_.reset();

        if(fencePool() != nullptr && fence_){
            fencePool()->release(std::move(fence_));
        }
        fence_.reset();
    }

    bool LayerBlurScratch::resize(unsigned width, unsigned height,
                                  OmegaGTE::PixelFormat pixelFormat){
        const unsigned w = std::max(1u, width);
        const unsigned h = std::max(1u, height);
        if(w == width_ && h == height_ && pixelFormat == pixelFormat_ && valid()){
            return false;
        }

        release();

        width_ = w;
        height_ = h;
        pixelFormat_ = pixelFormat;

        auto key = makeKey(w, h, pixelFormat);

        // Source: rendered into via a TextureRenderTarget; sampled by the
        // composite quad.
        if(texturePool() != nullptr){
            source_ = texturePool()->acquire(key);
        }
        else {
            OmegaGTE::TextureDescriptor desc {OmegaGTE::GETexture::Texture2D};
            desc.usage = OmegaGTE::GETexture::RenderTarget;
            desc.storage_opts = OmegaGTE::Shared;
            desc.width = w;
            desc.height = h;
            desc.pixelFormat = pixelFormat;
            source_ = gte.graphicsEngine->makeTexture(desc);
        }
        if(source_ == nullptr){
            release();
            return true;
        }

        // PingPong: compute output for the blur passes.
        if(texturePool() != nullptr){
            pingPong_ = texturePool()->acquire(key);
        }
        else {
            OmegaGTE::TextureDescriptor desc {OmegaGTE::GETexture::Texture2D};
            desc.usage = OmegaGTE::GETexture::RenderTarget;
            desc.storage_opts = OmegaGTE::Shared;
            desc.width = w;
            desc.height = h;
            desc.pixelFormat = pixelFormat;
            pingPong_ = gte.graphicsEngine->makeTexture(desc);
        }
        if(pingPong_ == nullptr){
            release();
            return true;
        }

        sourceTarget_ = gte.graphicsEngine->makeTextureRenderTarget({true, source_});
        if(sourceTarget_ == nullptr){
            release();
            return true;
        }

        if(fencePool() != nullptr){
            fence_ = fencePool()->acquire();
        }
        else {
            fence_ = gte.graphicsEngine->makeFence();
        }

        ResourceTrace::emit("Create",
                            "BlurScratch",
                            0,
                            "LayerBlurScratch",
                            this,
                            static_cast<float>(w),
                            static_cast<float>(h),
                            1.f);
        return true;
    }

}
