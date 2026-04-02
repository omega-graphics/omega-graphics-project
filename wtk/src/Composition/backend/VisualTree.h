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
            Core::Position pos;
            BackendRenderTargetContext renderTarget;
            explicit Visual(Core::Position & pos,BackendRenderTargetContext & renderTarget):
            pos(pos),
            renderTarget(renderTarget){
                traceResourceId = ResourceTrace::nextResourceId();
                ResourceTrace::emit("Create",
                                    "BackendVisual",
                                    traceResourceId,
                                    "BackendVisualTree::Visual",
                                    this);
            }
            virtual void resize(Core::Rect & newRect) = 0;
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
        INTERFACE_METHOD Core::SharedPtr<Visual> makeRootVisual(Core::Rect & rect,Core::Position & pos,
                                                                 ViewPresentTarget & outPresentTarget) ABSTRACT;
        /// Create a surface-only visual (GPU texture, no native present surface).
        INTERFACE_METHOD Core::SharedPtr<Visual> makeSurfaceVisual(Core::Rect & rect,Core::Position & pos) ABSTRACT;
        INTERFACE_METHOD void setRootVisual(Core::SharedPtr<Visual> & visual) ABSTRACT;
        INTERFACE_METHOD ~BackendVisualTree() = default;
    };

    

};

#endif
