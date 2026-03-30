#include "VisualTree.h"
#include <mutex>

#ifndef OMEGAWTK_COMPOSITION_BACKEND_RESOURCEFACTORY_H
#define OMEGAWTK_COMPOSITION_BACKEND_RESOURCEFACTORY_H

namespace OmegaWTK::Composition {

    /// Centralizes GPU resource creation and ensures it happens on the main thread.
    /// Resources that interact with the platform display pipeline (CAMetalLayer,
    /// swap chains, native render targets, offscreen textures for the blit pass)
    /// must be created on the main thread to integrate correctly with
    /// Core Animation / DComp / the window server.
    class BackendResourceFactory {
    public:

        struct VisualTreeBundle {
            SharedHandle<BackendVisualTree> visualTree;
            Core::SharedPtr<BackendVisualTree::Visual> rootVisual;
            BackendRenderTargetContext *rootContext = nullptr;
        };

        /// Creates a BackendVisualTree for the given ViewRenderTarget,
        /// with a root visual sized to `rect`.
        /// All underlying GPU resources (CAMetalLayer, GENativeRenderTarget,
        /// offscreen textures) are created on the main thread.
        VisualTreeBundle createVisualTreeForView(SharedHandle<ViewRenderTarget> & renderTarget,
                                                  Core::Rect & rect);

        /// Creates an offscreen texture + texture render target pair
        /// on the main thread.
        struct TextureTargetBundle {
            SharedHandle<OmegaGTE::GETexture> texture;
            SharedHandle<OmegaGTE::GETextureRenderTarget> renderTarget;
        };
        TextureTargetBundle createTextureTarget(unsigned width, unsigned height,
                                                 OmegaGTE::TexturePixelFormat format);

        /// Creates a child visual within an existing visual tree
        /// on the main thread. Used for element layers within a View.
        Core::SharedPtr<BackendVisualTree::Visual> createChildVisual(
                BackendVisualTree & tree,
                Core::Rect & rect);

        /// Creates a root visual within an existing visual tree
        /// on the main thread. Used as a fallback when the compositor
        /// thread encounters a tree that has no root visual yet.
        Core::SharedPtr<BackendVisualTree::Visual> createRootVisual(
                BackendVisualTree & tree,
                Core::Rect & rect);
    };

    /// Holds pre-created visual tree resources for a View.
    /// Populated during View construction on the main thread.
    struct PreCreatedVisualTreeData {
        BackendResourceFactory::VisualTreeBundle bundle;
    };

    /// Thread-safe registry mapping render targets to their pre-created
    /// visual tree data. Populated by View constructors (main thread),
    /// consumed by the compositor thread in ensureLayerSurfaceTarget.
    class PreCreatedResourceRegistry {
        static std::mutex mutex_;
        static OmegaCommon::Map<CompositionRenderTarget *, PreCreatedVisualTreeData *> registry_;
    public:
        static void store(CompositionRenderTarget *key, PreCreatedVisualTreeData *data);
        static PreCreatedVisualTreeData * lookup(CompositionRenderTarget *key);
        static void remove(CompositionRenderTarget *key);
    };

};

#endif
