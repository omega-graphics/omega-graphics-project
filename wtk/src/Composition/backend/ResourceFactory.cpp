#include "ResourceFactory.h"
#include "Pipeline.h"
#include "TexturePool.h"
#include "BufferPool.h"
#include "FencePool.h"
#include "BitmapTextureCache.h"
#include "MainThreadDispatch.h"
#include "omegaWTK/Core/Core.h"

namespace OmegaWTK::Composition {

    BackendResourceFactory::BackendResourceFactory()
        : pipelines_(std::make_unique<PipelineRegistry>())
    {}

    BackendResourceFactory::~BackendResourceFactory() = default;

    BackendResourceFactory & BackendResourceFactory::instance(){
        static BackendResourceFactory factory;
        return factory;
    }

    bool BackendResourceFactory::initializePools(){
        // Tear any previous pool state down first — matches the legacy
        // createResourcePools() contract, which assumed the caller had
        // already drained anything outstanding.
        shutdownPools();

        OmegaGTE::HeapDescriptor texHeapDesc {};
        texHeapDesc.len = kTextureHeapSize;
        OmegaGTE::HeapDescriptor bufHeapDesc {};
        bufHeapDesc.len = kBufferHeapSize;

        textureHeap_ = gte.graphicsEngine->makeHeap(texHeapDesc);
        bufferHeap_  = gte.graphicsEngine->makeHeap(bufHeapDesc);
        texturePool_ = std::make_unique<TexturePool>(textureHeap_);
        bufferPool_  = std::make_unique<BufferPool>(bufferHeap_);
        fencePool_   = std::make_unique<FencePool>();

        return texturePool_ != nullptr
            && bufferPool_  != nullptr
            && fencePool_   != nullptr;
    }

    void BackendResourceFactory::shutdownPools(){
        if(fencePool_)   fencePool_->drain();
        if(bufferPool_)  bufferPool_->drain();
        if(texturePool_) texturePool_->drain();
        fencePool_.reset();
        bufferPool_.reset();
        texturePool_.reset();
        bufferHeap_.reset();
        textureHeap_.reset();
        effectProcessor_.reset();
        // Phase 6.6: drop any cached BitmapImage uploads. Their textures
        // were allocated against the GraphicsEngine directly (not from
        // texturePool_), so they survive the pool drain — we have to
        // explicitly clear the cache here.
        BitmapTextureCache::instance().clear();
    }

    SharedHandle<BackendCanvasEffectProcessor> &
    BackendResourceFactory::effectProcessor(){
        if(effectProcessor_ == nullptr){
            effectProcessor_ = BackendCanvasEffectProcessor::Create();
        }
        return effectProcessor_;
    }

    // §2.14 Pass 1 retired `createVisualTreeForView`,
    // `createChildVisual`, `createRootVisual`, and the matching
    // `PreCreatedResourceRegistry`. Every backend now constructs its
    // per-window `Native::VisualTree` in the AppWindow ctor via
    // `Native::make_native_visual_tree`, and the compositor's
    // `attachVisualTree` owns the per-Visual
    // `BackendRenderTargetContext` through the per-backend
    // `tryBindRootVisual` (see `backend/<plat>/<Plat>VisualBinder`).

    BackendResourceFactory::TextureTargetBundle
    BackendResourceFactory::createTextureTarget(
            unsigned width, unsigned height,
            OmegaGTE::TexturePixelFormat format)
    {
        TextureTargetBundle result {};

        runOnMainThread([&]{
            OmegaGTE::TextureDescriptor desc {};
            desc.usage = OmegaGTE::GETexture::RenderTarget;
            desc.storage_opts = OmegaGTE::Shared;
            desc.width = width;
            desc.height = height;
            desc.kind = OmegaGTE::TextureKind::Tex2D;
            desc.pixelFormat = format;

            result.texture = gte.graphicsEngine->makeTexture(desc);
            if(result.texture != nullptr){
                result.renderTarget = gte.graphicsEngine->makeTextureRenderTarget(
                        {true, result.texture});
            }
        });

        return result;
    }

    // §2.14 Pass 1 — see the note above the TextureTargetBundle impl.

};
