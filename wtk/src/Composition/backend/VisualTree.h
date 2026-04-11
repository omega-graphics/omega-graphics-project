#include "../Compositor.h"
#include "RenderTarget.h"
#include "ResourceTrace.h"

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
            BackendRenderTargetContext renderTarget;
            explicit Visual(Composition::Point2D & pos,BackendRenderTargetContext & renderTarget):
            pos(pos),
            renderTarget(renderTarget){
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
        INTERFACE_METHOD ~BackendVisualTree() = default;
    };

    

};

#endif
