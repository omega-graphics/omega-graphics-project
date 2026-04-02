#include "../RenderTarget.h"
#include "../VisualTree.h"

#include <dcomp.h>

#pragma comment(lib,"dcomp.lib")


#ifndef OMEGAWTK_COMPOSITION_DX_DCVISUALTREE_H
#define OMEGAWTK_COMPOSITION_DX_DCVISUALTREE_H

namespace OmegaWTK::Composition {
/**
     DirectX Backend Impl of the BDCompositionVisualTree using IDCompositionVisuals
*/
class DCVisualTree : public BackendVisualTree {
    Core::UniqueComPtr<IDCompositionTarget> hwndTarget;
    float renderScale = 1.f;
    typedef BackendVisualTree Parent;
    public:

    /// Root visual — owns IDCompositionVisual2 for DComp tree.
    /// The IDXGISwapChain3 is owned by ViewPresentTarget, not this struct.
    struct RootVisual : public Parent::Visual {
        IDCompositionVisual2 * visual;
        float renderScale = 1.f;
        explicit RootVisual(Core::Position & pos,
                        BackendRenderTargetContext &context,
                        IDCompositionVisual2 * visual,
                        float renderScale);
        void resize(Core::Rect &newRect) override;
        ~RootVisual() override;
    };

    /// Surface-only visual — GPU texture, no swap chain or DComp visual.
    struct SurfaceVisual : public Parent::Visual {
        float renderScale = 1.f;
        explicit SurfaceVisual(Core::Position & pos,
                        BackendRenderTargetContext &context,
                        float renderScale);
        void resize(Core::Rect &newRect) override;
    };

    explicit DCVisualTree(SharedHandle<ViewRenderTarget> & view);
    void addVisual(Core::SharedPtr<Parent::Visual> & visual) override;
    Core::SharedPtr<Parent::Visual> makeRootVisual(Core::Rect & rect,Core::Position & pos,
                                                    ViewPresentTarget & outPresentTarget) override;
    Core::SharedPtr<Parent::Visual> makeSurfaceVisual(Core::Rect & rect,Core::Position & pos) override;
    void setRootVisual(Core::SharedPtr<Parent::Visual> & visual) override;

};

};

#endif
