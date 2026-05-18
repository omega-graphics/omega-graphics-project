#include "../Compositor.h"
#include "RenderTarget.h"
#include "ResourceTrace.h"
#include <memory>
#include <utility>

#ifndef OMEGAWTK_COMPOSITION_BACKEND_VISUALTREE_H
#define OMEGAWTK_COMPOSITION_BACKEND_VISUALTREE_H

namespace OmegaWTK::Composition {
    /**
     @brief The Interface for rendering LayerTreeLimbs
    */
    class BackendResourceFactory;

    INTERFACE BackendVisualTree {
        friend class BackendResourceFactory;
    protected:

        struct Visual {
            std::uint64_t traceResourceId = 0;
            Composition::Point2D pos;
            // Owned indirectly so the back-references inside
            // BackendRenderTargetContext (FrameRenderPass / sizing state)
            // remain bound to a stable address for the lifetime of the Visual.
            std::unique_ptr<BackendRenderTargetContext> renderTarget;
            explicit Visual(Composition::Point2D & pos,
                            std::unique_ptr<BackendRenderTargetContext> renderTarget):
            pos(pos),
            renderTarget(std::move(renderTarget)){
                traceResourceId = ResourceTrace::nextResourceId();
                ResourceTrace::emit("Create",
                                    "BackendVisual",
                                    traceResourceId,
                                    "BackendVisualTree::Visual",
                                    this);
            }
            virtual void resize(Composition::Rect & newRect) = 0;
            virtual ~Visual(){
                ResourceTrace::emit("Destroy",
                                    "BackendVisual",
                                    traceResourceId,
                                    "BackendVisualTree::Visual",
                                    this);
            }
        };
    public:
        Core::SharedPtr<Visual> root = nullptr;
        OmegaCommon::Vector<Core::SharedPtr<Visual>> body;
        bool hasRootVisual(){
            return (bool)root;
        };
        static SharedHandle<BackendVisualTree> Create(SharedHandle<ViewRenderTarget> & view);
        INTERFACE_METHOD void addVisual(Core::SharedPtr<Visual> & visual) ABSTRACT;
        /// Create the root visual with a native present surface (swap chain / CAMetalLayer).
        /// The native render target is stored in the ViewPresentTarget, not in the Visual's context.
        INTERFACE_METHOD Core::SharedPtr<Visual> makeRootVisual(Composition::Rect & rect,Composition::Point2D & pos,
                                                                 ViewPresentTarget & outPresentTarget) ABSTRACT;
        /// Create a surface-only visual (GPU texture, no native present surface).
        INTERFACE_METHOD Core::SharedPtr<Visual> makeSurfaceVisual(Composition::Rect & rect,Composition::Point2D & pos) ABSTRACT;
        INTERFACE_METHOD void setRootVisual(Core::SharedPtr<Visual> & visual) ABSTRACT;
        /// Called lazily on first render when the native present target was unavailable at
        /// construction time (e.g. GTK widget not yet anchored to a toplevel).
        virtual void resolveDeferredNativeTarget(ViewPresentTarget &) {}

        /// Tier 3 Phase 3.7: drain the per-frame native-content
        /// carve-outs that `BackendRenderTargetContext::renderToTarget`
        /// recorded into `ctx.pendingNativeContent()` and translate
        /// each record into the platform-specific native-layer
        /// ordering primitive (CALayer sublayer on macOS, DComp
        /// visual on Windows, Wayland subsurface or X11 child on
        /// Linux). The compositor's frame worker calls this after
        /// it walks the frame's slices but before present. The
        /// default impl just clears the list — platforms with a
        /// hostId → native-layer registry (lit by `NativeViewHost-
        /// Adoption-Plan.md` Phases V2 / G2) override and perform
        /// the actual primitive insertion. Safe to call when the
        /// registry is empty; safe to call with zero pending
        /// carve-outs (no-op).
        virtual void applyNativeContentCarveouts(BackendRenderTargetContext & ctx){
            ctx.clearPendingNativeContent();
        }
        INTERFACE_METHOD ~BackendVisualTree() = default;
    };

    

};

#endif
