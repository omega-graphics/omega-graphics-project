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

    BackendResourceFactory::VisualTreeBundle
    BackendResourceFactory::createVisualTreeForView(
            SharedHandle<ViewRenderTarget> & renderTarget,
            Composition::Rect & rect,
            ViewPresentTarget & outPresentTarget)
    {
        VisualTreeBundle result {};

        runOnMainThread([&]{
            result.visualTree = BackendVisualTree::Create(renderTarget);
            result.rootVisual = result.visualTree->makeRootVisual(rect, rect.pos, outPresentTarget);
            // If the platform couldn't resolve a native present surface
            // yet (e.g. GTK/Vulkan: the GdkWindow isn't realized until the
            // toplevel is shown), discard the partial root visual. The
            // BackendRenderTargetContext we just built holds a null
            // GENativeRenderTarget — committing it would crash. Compositor
            // ::renderCompositeFrame's first-frame fallback recreates the
            // root visual via createRootVisual() once the surface is
            // resolvable, by which point the window has been displayed.
            if(outPresentTarget.nativeTarget == nullptr){
                result.rootVisual.reset();
                result.rootContext = nullptr;
            }
            else {
                result.visualTree->setRootVisual(result.rootVisual);
                result.rootContext = result.rootVisual->renderTarget.get();
            }
        });

        return result;
    }

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

    Core::SharedPtr<BackendVisualTree::Visual>
    BackendResourceFactory::createChildVisual(
            BackendVisualTree & tree,
            Composition::Rect & rect)
    {
        Core::SharedPtr<BackendVisualTree::Visual> result;

        runOnMainThread([&]{
            result = tree.makeSurfaceVisual(rect, rect.pos);
            tree.addVisual(result);
        });

        return result;
    }

    Core::SharedPtr<BackendVisualTree::Visual>
    BackendResourceFactory::createRootVisual(
            BackendVisualTree & tree,
            Composition::Rect & rect,
            ViewPresentTarget & outPresentTarget)
    {
        Core::SharedPtr<BackendVisualTree::Visual> result;

        runOnMainThread([&]{
            result = tree.makeRootVisual(rect, rect.pos, outPresentTarget);
            tree.setRootVisual(result);
        });

        return result;
    }

    // --- PreCreatedResourceRegistry ---

    std::mutex PreCreatedResourceRegistry::mutex_;
    OmegaCommon::Map<CompositionRenderTarget *, PreCreatedVisualTreeData *>
            PreCreatedResourceRegistry::registry_;

    void PreCreatedResourceRegistry::store(CompositionRenderTarget *key,
                                           PreCreatedVisualTreeData *data){
        std::lock_guard<std::mutex> lock(mutex_);
        registry_[key] = data;
    }

    PreCreatedVisualTreeData * PreCreatedResourceRegistry::lookup(
            CompositionRenderTarget *key){
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = registry_.find(key);
        if(it != registry_.end()){
            return it->second;
        }
        return nullptr;
    }

    void PreCreatedResourceRegistry::remove(CompositionRenderTarget *key){
        std::lock_guard<std::mutex> lock(mutex_);
        registry_.erase(key);
    }

};
