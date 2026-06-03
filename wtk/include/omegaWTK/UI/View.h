/**
 @file View.h

 */

#include "omegaWTK/Native/NativeEvent.h"
#include "omegaWTK/Composition/Geometry.h"

#include <cstdint>
#include <functional>
#include <limits>

#ifndef OMEGAWTK_UI_VIEW_H
#define OMEGAWTK_UI_VIEW_H

namespace OmegaWTK {
    // Forward decl — LayoutManager.h includes View.h (for ResizeClamp /
    // ChildResizeSpec); the back-edge would be circular if pulled in.
    class LayoutManager;
    namespace Composition {
        class Compositor;
        class CompositorClientProxy;
        class ViewRenderTarget;
        class LayerTree;
        class Layer;
        class ViewAnimator;
        // Phase 4.7.0: forward-declared so `View::paint` takes the
        // existing Tier-B / B3 scaffolding context by reference without
        // dragging DisplayList.h into the View public header.
        struct PaintContext;
    }

    namespace Native {
        class NativeEvent;
        typedef SharedHandle<NativeEvent> NativeEventPtr;
    }


    class Container;
    class Widget;
    class ViewDelegate;
    class ScrollView;
    class View;
    OMEGACOMMON_SHARED_CLASS(View);

    /// UIView-Render-Redesign-Plan Tier 2 Phase 2.5: per-view signal
    /// that fires when the view's layout rect resolves to a new
    /// value. Subscribers get the new rect in parent-relative
    /// coordinates. Fires on rect changes only — 3D-effect transform
    /// pushes (see `DrawOp::SetTransform`) do not trigger this signal
    /// (a separate `onTransformChanged` is deferred to Tier 3 if a
    /// use case appears). The canonical subscriber is
    /// `NativeViewHost`, which uses the signal to sync its embedded
    /// native item's bounds without the FrameBuilder gaining a
    /// per-node commit callback.
    class OMEGAWTK_EXPORT LayoutResolvedSignal {
    public:
        using Callback = std::function<void(const Composition::Rect &)>;
        void subscribe(Callback cb) { callbacks_.push_back(std::move(cb)); }
        void emit(const Composition::Rect & rect) const {
            for (const auto & cb : callbacks_) cb(rect);
        }
    private:
        OmegaCommon::Vector<Callback> callbacks_;
    };

    struct OMEGAWTK_EXPORT ResizeClamp {
        float minWidth = 1.f;
        float minHeight = 1.f;
        float maxWidth = std::numeric_limits<float>::infinity();
        float maxHeight = std::numeric_limits<float>::infinity();
    };

    enum class ChildResizePolicy : std::uint8_t {
        Fixed,
        Fill,
        FitContent,
        Proportional
    };

    struct OMEGAWTK_EXPORT ChildResizeSpec {
        bool resizable = true;
        ChildResizePolicy policy = ChildResizePolicy::FitContent;
        ResizeClamp clamp {};
        float growWeightX = 1.f;
        float growWeightY = 1.f;
    };

    // Phase 4.5: `ViewResizeCoordinator` is deleted. Child layout is
    // owned by the parent's `LayoutManager` (see `LayoutManager.h`);
    // the static `clampRectToParent` utility moved to
    // `LayoutManager::clampRectToParent`. `ChildResizeSpec` / the
    // policies / `ResizeClamp` above stay public for the surviving
    // call sites that pass them to the clamp helper.



    /**
        @brief Controls all the basic functionality of a Widget!
        @relates Widget
     */
    class OMEGAWTK_EXPORT View : public Native::NativeEventEmitter {
    protected:
        Composition::CompositorClientProxy & compositorProxy();
        const Composition::CompositorClientProxy & compositorProxy() const;
        friend class Widget;
    private:
        struct Impl;
        Core::UniquePtr<Impl> impl_;
        SharedHandle<Composition::ViewRenderTarget> & renderTargetHandle();
        const SharedHandle<Composition::ViewRenderTarget> & renderTargetHandle() const;
        void setFrontendRecurse(Composition::Compositor *frontend);
        void setSyncLaneRecurse(uint64_t syncLaneId);
        virtual bool hasDelegate();
        void addSubView(View *view);
        void removeSubView(View * view);
        friend class AppWindow;
        // Phase 4.8: `Composition::ViewAnimator` friend deleted alongside
        // the class itself (the pre-scheduler animation runtime).
        friend class ScrollView;
        friend class Widget;
        friend class WidgetTreeHost;
        friend class Container;
    protected:
        /**
            Constructs a View. Creates its own LayerTree with a root Layer.
            @param rect The Rect to use
            @param parent The parent View (nullptr for root views)
            @returns A View!
         */
        View(const Composition::Rect & rect,ViewPtr parent = nullptr);
    public:
        OMEGACOMMON_CLASS("OmegaWTK.View")

        /// Creates a View. Public factory for use by Widget subclass constructors.
        static ViewPtr Create(const Composition::Rect & rect,ViewPtr parent = nullptr){
            return ViewPtr(new View(rect,parent));
        }

        /// @brief Retrieves the Rect that defines the position and bounds of the View.
        Composition::Rect & getRect();
        // Phase 4.8: `getLayerTree()` and the per-view
        // `View::Impl::ownLayerTree` are gone. Every UIView's
        // DisplayList flows into the single
        // `AppWindow::Impl::windowLayerTree_` via
        // `FrameBuilder::buildFrame` now. Callers that needed a
        // tree handle pre-4.8 (legacy compositor observation, the
        // dormant per-tag animator, the resize fallback in
        // `UIView::Update.cpp::localBoundsFromView`) all read from
        // `View::getRect()` or the window-level state instead.

        /// Widget-View-Paint-Lifecycle-Plan Tier A: per-node dirty
        /// state. `invalidate()` sets these bits and defers the actual
        /// paint to the next frame boundary instead of painting inline.
        /// The bits are an unscoped enum so callers can OR them
        /// (`View::Paint | View::Layout`) per the plan's §3.3.
        enum DirtyBit : uint8_t {
            Style   = 1 << 0,
            Layout  = 1 << 1,
            Content = 1 << 2,
            Paint   = 1 << 3,
        };
        /// OR `bits` into this view's dirty mask. Phase 4.7.3: also
        /// walks the parent chain to the root, OR-ing `bits` into
        /// each ancestor's *descendant-dirty* mask so
        /// `FrameBuilder::buildFrame` can read the root and know
        /// "any node anywhere needs this pass". The propagated mask
        /// is distinct from each node's own `dirtyBits()` — gating
        /// uses the union of the two.
        void markDirty(uint8_t bits);
        /// Current self-only dirty mask (combination of DirtyBit
        /// values). Excludes the propagated descendant mask.
        uint8_t dirtyBits() const;
        /// Phase 4.7.3: OR of every descendant's `dirtyBits()`.
        /// Maintained incrementally by `markDirty()`; cleared
        /// together with `dirtyBits_` by `clearDirtyBits()`.
        uint8_t descendantDirty() const;
        /// Reset BOTH the self mask AND the propagated descendant
        /// mask to zero (Phase 4.7.3 — pre-4.7.3 this cleared only
        /// the self mask).
        void clearDirtyBits();
        /// @brief Checks to see if this View is the root View of a Widget.
        bool isRootView();
        /// Phase 4.5: parent-owned child-layout strategy. The manager
        /// arranges THIS view's children (`subviews()`); it does not
        /// touch intra-`UIView` element layout. Default is the process-
        /// wide `AbsoluteLayout` singleton (child positioned by its own
        /// rect, clamped to parent). Set via `setLayoutManager(...)`; the
        /// caller owns the manager's lifetime (the View stores a raw
        /// pointer). Returns a pointer rather than a reference so callers
        /// can detect the "default singleton" case if they want.
        LayoutManager * layoutManager() const;
        void            setLayoutManager(LayoutManager * manager);

        /// Phase 4.5: const view of this View's children. Used by
        /// `LayoutManager::arrange` to iterate. Order is insertion order.
        OmegaCommon::ArrayRef<View *> subviews() const;

        /// @brief Sets the object to recieve View related events.
        virtual void setDelegate(ViewDelegate *_delegate);

        /// @brief Returns true if `point` (in parent-relative coordinates)
        /// falls within this View's rect.
        bool containsPoint(const Composition::Point2D &point) const;

        /// @brief Resize the view synchronously.
        /// @note If you wish to animate the View resize, please use the ViewAnimator to perform that action.
        /// @note Fires `onLayoutResolved` when the sanitized rect
        /// actually differs from the prior rect; same-rect calls are
        /// no-ops (no signal).
        virtual void resize(Composition::Rect newRect);

        /// Per-view layout-rect-resolved signal (Phase 2.5). Fires
        /// from `resize()` only on actual rect changes. Subscribers
        /// (today: `NativeViewHost`) get the new parent-relative
        /// rect. Public so external code can attach without the
        /// FrameBuilder owning a per-node commit hook.
        LayoutResolvedSignal onLayoutResolved;

        // Phase 4.7.5: `startCompositionSession` / `endCompositionSession`
        // are deleted. They paired with the per-view canvas /
        // submitView path that 4.7.4 retired (the window-level
        // FrameBuilder owns the composition session now; the proxy is
        // propagated at `addSubView` time so no per-view session-open
        // dance is needed).

        // (Phase 4.7.5: `endCompositionSession` deleted alongside
        // `startCompositionSession` — see note above.)

        /// @brief Make the View visible.
        void enable();

        /// @brief Make the View invisible.
        void disable();

        /// @brief Returns true if the View is enabled (visible).
        bool isEnabled() const;

        /// @brief Set the shared window render target for this View and
        /// all subviews. Called by WidgetTreeHost when the widget tree
        /// attaches to an AppWindow (Phase 3, single-surface rendering).
        void setWindowRenderTarget(SharedHandle<Composition::ViewRenderTarget> windowRT);

        /// @brief Logical-to-physical pixel scale factor for this view's
        /// window (1.0 = 96 DPI). Sourced from the attached ViewRenderTarget.
        /// Returns 1.0 if no render target is attached yet.
        float getRenderScale() const;

        /// Phase 4.7.5: this View's absolute position relative to
        /// the window origin, computed by walking the parent chain
        /// (sum of each ancestor's `rect.pos + parent.contentOffset()`).
        /// Pre-4.7 was the `legacyComputeWindowOffset` companion to
        /// the now-deleted FrameBuilder offset accumulator; the
        /// public `computeWindowOffset` wrapper / accumulator are
        /// gone (paint reads `PaintContext.offset` from
        /// `FrameBuilder::buildFrame`'s walker, never via a View
        /// accessor). One in-tree caller survives:
        /// `NativeViewHost::syncBounds`, which fires from
        /// `onLayoutResolved` to position an embedded native item
        /// against the window — that path runs OUTSIDE the paint
        /// walker so it cannot read PaintContext, and a parent-walk
        /// is the right hammer. Renamed from `legacyComputeWindowOffset`
        /// to drop the "legacy" tag and reflect the new contract
        /// ("for embed-sync use, not paint").
        Composition::Point2D offsetFromRoot() const;

        // Phase 4.7.5: `computeWindowOffset` / `legacyComputeWindowOffset`
        // / `scrollOffsetContribution` are deleted. The paint walker
        // owns the absolute-position math now (via `PaintContext.offset`);
        // `contentOffset()` is what ScrollView overrides for the
        // descent walk (Tier 3 Phase 3.6 supersession).

        /// Tier 3 Phase 3.6: the offset this View applies to *its
        /// children's* positions when arranging them. Sign convention:
        /// `contentOffset` is added during the offset walk, whereas
        /// `scrollOffsetContribution` was subtracted, so ScrollView
        /// returns `-scrollOffset_`. Non-scrolling Views return {0,0}.
        /// `View::legacyComputeWindowOffset` and (via it) the
        /// FrameBuilder offset accumulator (Phase 3.4 stack) read this
        /// when entering a subtree so descendant `finalRect`s are
        /// observed scroll-shifted in the layout pipeline rather than
        /// at paint time.
        virtual Composition::Point2D contentOffset() const;

        /// Tier 3 Phase 3.6: subtree-layerization tag. Returning true
        /// hints to the compositor that this subtree's DisplayList
        /// output should be tagged for a separate composition layer
        /// (future compositor-thread scrolling / retained content
        /// texture). Tier 3 does NOT yet act on the tag — content
        /// re-rasterizes every frame — but the surface is in place
        /// for a later compositor pass. ScrollView is the canonical
        /// `true` returner.
        virtual bool wantsLayer() const;

        void applyLayoutDelta(const struct LayoutDelta & delta,
                              const struct LayoutTransitionSpec & spec);

        /// Phase 4.4: stable per-`View` identity used by the per-window
        /// `AnimationScheduler` as the `NodeId` for property animations
        /// (the View's own layout tweens — see `applyLayoutDelta`). Plain
        /// `std::uint64_t` (the alias `OmegaWTK::NodeId` lives in the
        /// private animation header) so the public `View` surface does
        /// not depend on the scheduler header. Generated once per `View`,
        /// stable for the View's lifetime, never reused.
        std::uint64_t nodeId() const;

        /// Widget-View-Paint-Lifecycle-Plan Tier D / D6.4 (2026-06-03):
        /// pseudo-class state bitmask. Bit layout matches the
        /// `OmegaWTK::StyleSheets::PseudoClass` enum (Hover=1,
        /// Pressed=2, Focused=4, Disabled=8) but exposed as a raw
        /// `std::uint8_t` so the `View` surface doesn't drag the
        /// sheet vocabulary into every includer. The
        /// `WidgetTreeHost` input dispatcher writes these on hover-
        /// change and mouse-button transitions; `setEnabled()` flips
        /// the Disabled bit. The `StyleSheets::StyleResolver` reads
        /// them during selector match.
        std::uint8_t pseudoClassBits() const;
        /// Set or clear the bits named in `mask`. `markDirty(Style)`
        /// is called if anything actually changes, so the next frame
        /// re-resolves through the cascade with the new state.
        void setPseudoClassBits(std::uint8_t mask, bool on);

        /// Phase 4.7.0: the polymorphic Paint-pass hook. Per-node:
        /// emits THIS view's draw ops into `pc.displayList` and reads
        /// `pc.offset` for absolute window positioning. Does NOT recurse
        /// — subtree traversal lives in `FrameBuilder::buildFrame`'s
        /// walker (4.7.1), which calls this once per node. The default
        /// (base `View`) is a pass-through no-op; `UIView`, `SVGView`,
        /// `ScrollView` override to emit their ops.
        virtual void paint(Composition::PaintContext & pc);

        /// Phase 4.7.2: the polymorphic Style-pass hook. Per-node:
        /// resolves this view's style into its private cache (read by
        /// the subsequent Paint pass). Default is a no-op (base `View`s
        /// have no style of their own). `UIView` overrides to run the
        /// `resolveViewStyle` + per-element resolution into the
        /// per-property `styleTable_` (Tier D / D5, 2026-06-03 — pre-D5
        /// this populated the `resolvedViewStyle_` + `computedStyles_`
        /// aggregate cache). Called once per dirty node by
        /// `FrameBuilder::buildFrame`'s Style pass before Layout / Paint.
        virtual void resolveStyles();

        /// Phase 4.7.2: the polymorphic Layout-pass hook for *intra-node*
        /// arrangement. Distinct from `LayoutManager::arrange`, which
        /// arranges this view's *child views*: `arrangeContent()` runs
        /// the inside-this-node layout work — for `UIView`, that is the
        /// element-rect resolution from `UIViewLayoutV2` into
        /// `impl_->arranged_`. Default is a no-op (base `View`s have no
        /// internal elements). Called once per dirty node by
        /// `FrameBuilder::buildFrame`'s Layout pass *after* the
        /// child-axis `LayoutManager` measure/arrange.
        virtual void arrangeContent();

        virtual ~View();
    };

    /**
        @brief The Root View delegate class!
     */
    INTERFACE OMEGAWTK_EXPORT ViewDelegate : public Native::NativeEventProcessor {
        void onRecieveEvent(Native::NativeEventPtr event);
        ViewDelegate *forwardDelegate = nullptr;
        friend class View;
        protected:
        View * view;

        void setForwardDelegate(ViewDelegate *delegate);
        /**
            Called when the Mouse Enters the View
         */
        virtual void onMouseEnter(Native::NativeEventPtr event) DEFAULT;
        /**
            Called when the Mouse Exits the View
         */
        virtual void onMouseExit(Native::NativeEventPtr event) DEFAULT;
        /**
            Called when the Left Mouse Button is pressed
         */
        virtual void onLeftMouseDown(Native::NativeEventPtr event) DEFAULT;
        /**
            Called when the Left Mouse Button is raised after being pressed
         */
        virtual void onLeftMouseUp(Native::NativeEventPtr event) DEFAULT;
        /**
            Called when the Right Mouse Button is pressed
         */
        virtual void onRightMouseDown(Native::NativeEventPtr event) DEFAULT;
        /**
            Called when the Right Mouse Button is raised after being pressed
         */
        virtual void onRightMouseUp(Native::NativeEventPtr event) DEFAULT;
        /**
            Called when a key on a keyboard is pressed
         */
        virtual void onKeyDown(Native::NativeEventPtr event) DEFAULT;
        /**
            Called when a key on a keyboard is raised after being pressed
         */
        virtual void onKeyUp(Native::NativeEventPtr event) DEFAULT;
        public:
        ViewDelegate();
        ~ViewDelegate();
    };


};

#endif
