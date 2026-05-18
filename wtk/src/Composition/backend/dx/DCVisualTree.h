#include "../RenderTarget.h"
#include "../VisualTree.h"

#include "omegaWTK/Core/Microsoft.h"

#include <dcomp.h>
#include <memory>

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
        explicit RootVisual(Composition::Point2D & pos,
                        std::unique_ptr<BackendRenderTargetContext> context,
                        IDCompositionVisual2 * visual,
                        float renderScale);
        void resize(Composition::Rect &newRect) override;
        ~RootVisual() override;
    };

    /// Surface-only visual — GPU texture, no swap chain or DComp visual.
    struct SurfaceVisual : public Parent::Visual {
        float renderScale = 1.f;
        explicit SurfaceVisual(Composition::Point2D & pos,
                        std::unique_ptr<BackendRenderTargetContext> context,
                        float renderScale);
        void resize(Composition::Rect &newRect) override;
    };

    explicit DCVisualTree(SharedHandle<ViewRenderTarget> & view);
    void addVisual(Core::SharedPtr<Parent::Visual> & visual) override;
    Core::SharedPtr<Parent::Visual> makeRootVisual(Composition::Rect & rect,Composition::Point2D & pos,
                                                    ViewPresentTarget & outPresentTarget) override;
    Core::SharedPtr<Parent::Visual> makeSurfaceVisual(Composition::Rect & rect,Composition::Point2D & pos) override;
    void setRootVisual(Core::SharedPtr<Parent::Visual> & visual) override;

    /// Tier 3 Phase 3.7: drain the per-frame native-content carve-outs
    /// recorded by `BackendRenderTargetContext::renderToTarget` and
    /// translate each record into DirectComposition visual insertion
    /// against this tree's root `IDCompositionVisual2`. The hostId →
    /// `IDCompositionVisual2` registry is owned by `HWNDItem`
    /// (registered there by `NativeViewHost-Adoption-Plan.md` Phases
    /// V2 / G2); until that registry exists, this drain logs the
    /// records and clears the list so the next frame starts clean.
    /// Called by the compositor frame worker after the slice loop
    /// completes.
    void applyNativeContentCarveouts(BackendRenderTargetContext & ctx) override;

};

};

#endif
