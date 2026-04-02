#include "ResourceFactory.h"
#include "MainThreadDispatch.h"
#include "omegaWTK/Core/Core.h"

namespace OmegaWTK::Composition {

    BackendResourceFactory::VisualTreeBundle
    BackendResourceFactory::createVisualTreeForView(
            SharedHandle<ViewRenderTarget> & renderTarget,
            Core::Rect & rect,
            ViewPresentTarget & outPresentTarget)
    {
        VisualTreeBundle result {};

        runOnMainThread([&]{
            result.visualTree = BackendVisualTree::Create(renderTarget);
            result.rootVisual = result.visualTree->makeRootVisual(rect, rect.pos, outPresentTarget);
            result.visualTree->setRootVisual(result.rootVisual);
            result.rootContext = &result.rootVisual->renderTarget;
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
            desc.type = OmegaGTE::GETexture::Texture2D;
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
            Core::Rect & rect)
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
            Core::Rect & rect,
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
