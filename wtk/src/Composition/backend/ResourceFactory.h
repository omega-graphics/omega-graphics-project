// Single owner of every GPU resource the composition backend allocates.
//
// Holds the `PipelineRegistry`, the texture / buffer / fence pools and the
// heaps that back them, and the per-context effect-processor factory.
// Visual-tree creation (which must run on the main thread because of
// Core Animation / DComp / window server interactions) also flows through
// here. There is exactly one factory per process — `instance()` — and every
// other backend file reaches for it directly rather than carrying its own
// statics.

#include "VisualTree.h"
#include "Effect.h"
#include <memory>
#include <mutex>

#ifndef OMEGAWTK_COMPOSITION_BACKEND_RESOURCEFACTORY_H
#define OMEGAWTK_COMPOSITION_BACKEND_RESOURCEFACTORY_H

namespace OmegaWTK::Composition {

    class PipelineRegistry;
    class TexturePool;
    class BufferPool;
    class FencePool;

    /// Centralizes GPU resource creation and ensures it happens on the main thread.
    /// Resources that interact with the platform display pipeline (CAMetalLayer,
    /// swap chains, native render targets, offscreen textures for the blit pass)
    /// must be created on the main thread to integrate correctly with
    /// Core Animation / DComp / the window server.
    class BackendResourceFactory {
        std::unique_ptr<PipelineRegistry> pipelines_;

        SharedHandle<OmegaGTE::GEHeap> textureHeap_;
        SharedHandle<OmegaGTE::GEHeap> bufferHeap_;
        std::unique_ptr<TexturePool> texturePool_;
        std::unique_ptr<BufferPool> bufferPool_;
        std::unique_ptr<FencePool> fencePool_;
        SharedHandle<BackendCanvasEffectProcessor> effectProcessor_;

        static constexpr std::size_t kTextureHeapSize = 64u * 1024u * 1024u;
        static constexpr std::size_t kBufferHeapSize = 8u * 1024u * 1024u;
    public:
        BackendResourceFactory();
        ~BackendResourceFactory();

        /// Process-wide factory accessor. The factory is the single owner of
        /// the compositor's process-global GPU resources (pipelines, pools,
        /// fences, and gradient textures).
        static BackendResourceFactory & instance();

        /// Pipeline state objects, shader library, fullscreen quad buffer,
        /// per-format copy pipeline cache.
        PipelineRegistry & pipelines() { return *pipelines_; }

        /// Allocate the underlying heaps and construct the texture / buffer /
        /// fence pools. Safe to call multiple times — subsequent invocations
        /// tear the previous pools down first. Returns true if every pool was
        /// constructed successfully.
        bool initializePools();

        /// Drain and release every pool plus the heaps that back them.
        void shutdownPools();

        /// Pool accessors. Return nullptr until initializePools() has been
        /// called (matching the legacy `if(texturePool)` guard semantics).
        TexturePool * texturePool() { return texturePool_.get(); }
        BufferPool *  bufferPool()  { return bufferPool_.get(); }
        FencePool *   fencePool()   { return fencePool_.get(); }

        /// The process-wide canvas-effect processor. Stateless after the
        /// Phase 4.4 fence move — every per-layer blur composite borrows
        /// the same instance. Lazily constructed on first access; null
        /// when the backing implementation cannot be created.
        SharedHandle<BackendCanvasEffectProcessor> & effectProcessor();

        struct VisualTreeBundle {
            SharedHandle<BackendVisualTree> visualTree;
            Core::SharedPtr<BackendVisualTree::Visual> rootVisual;
            BackendRenderTargetContext *rootContext = nullptr;
        };

        /// Creates a BackendVisualTree for the given ViewRenderTarget,
        /// with a root visual sized to `rect`.
        /// The root visual's native present surface is stored in outPresentTarget.
        /// All underlying GPU resources are created on the main thread.
        VisualTreeBundle createVisualTreeForView(SharedHandle<ViewRenderTarget> & renderTarget,
                                                  Composition::Rect & rect,
                                                  ViewPresentTarget & outPresentTarget);

        /// Creates an offscreen texture + texture render target pair
        /// on the main thread.
        struct TextureTargetBundle {
            SharedHandle<OmegaGTE::GETexture> texture;
            SharedHandle<OmegaGTE::GETextureRenderTarget> renderTarget;
        };
        TextureTargetBundle createTextureTarget(unsigned width, unsigned height,
                                                 OmegaGTE::TexturePixelFormat format);

        /// Creates a texture-only child visual within an existing visual tree
        /// on the main thread. No native present surface.
        Core::SharedPtr<BackendVisualTree::Visual> createChildVisual(
                BackendVisualTree & tree,
                Composition::Rect & rect);

        /// Creates a root visual with a native present surface within
        /// an existing visual tree on the main thread.
        Core::SharedPtr<BackendVisualTree::Visual> createRootVisual(
                BackendVisualTree & tree,
                Composition::Rect & rect,
                ViewPresentTarget & outPresentTarget);
    };

    /// Holds pre-created visual tree resources for a View.
    /// Populated during View construction on the main thread.
    struct PreCreatedVisualTreeData {
        BackendResourceFactory::VisualTreeBundle bundle;
        ViewPresentTarget presentTarget;
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
