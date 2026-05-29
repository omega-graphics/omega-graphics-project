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
    namespace Composition {
        class Compositor;
        class CompositorClientProxy;
        class ViewRenderTarget;
        class LayerTree;
        class Layer;
        class ViewAnimator;
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

    class OMEGAWTK_EXPORT ViewResizeCoordinator {
        struct ChildState {
            ChildResizeSpec spec {};
            Composition::Rect baselineParentRect {Composition::Point2D{0.f,0.f},1.f,1.f};
            Composition::Rect baselineChildRect {Composition::Point2D{0.f,0.f},1.f,1.f};
            bool hasBaseline = false;
        };
        View * parentView = nullptr;
        std::uint64_t activeSessionId = 0;
        OmegaCommon::Map<View *,ChildState> childState;
    public:
        void attachView(View * parent);
        void registerChild(View * child,const ChildResizeSpec & spec);
        void updateChildSpec(View * child,const ChildResizeSpec & spec);
        void unregisterChild(View * child);
        void beginResizeSession(std::uint64_t sessionId);
        Composition::Rect resolveChildRect(View * child,
                                    const Composition::Rect & requested,
                                    const Composition::Rect & parentContentRect);
        void resolve(const Composition::Rect & parentContentRect);
        static Composition::Rect clampRectToParent(const Composition::Rect & requested,
                                            const Composition::Rect & parentContentRect,
                                            const ChildResizeSpec & spec);
    };
    

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
        friend class Composition::ViewAnimator;
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
        /// @brief Retrieves the View's own LayerTree.
        Composition::LayerTree * getLayerTree();

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
        /// OR `bits` into this view's dirty mask.
        void markDirty(uint8_t bits);
        /// Current dirty mask (combination of DirtyBit values).
        uint8_t dirtyBits() const;
        /// Reset the dirty mask to zero.
        void clearDirtyBits();
        /// @brief Checks to see if this View is the root View of a Widget.
        bool isRootView();
        /// @brief Returns the resize coordinator associated with this view.
        ViewResizeCoordinator & getResizeCoordinator();
        const ViewResizeCoordinator & getResizeCoordinator() const;

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

        /// @brief Starts a Composition Session for this View.
        /// @paragraph Upon invocation, this will allow Canvases to render to child Layers in the View's LayerTree
        /// and it will allow submission of render and animation commands from the child LayerAnimators and ViewAnimator.
        /// If one attempts to try animate or render to the View or any child Layers without calling this method FIRST, will recieve an access error.
        void startCompositionSession();

        /// @brief Ends a Composition Session for this View.
        /// @paragraph This method closes the submission queue of all render commands and submits them to the Compositor.
        /// Any commands posted to the CompositorClientProxy after invocation of this method will be ignored and an access error will be thrown.
        void endCompositionSession();

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

        /// @brief Compute this View's position relative to the window
        /// origin. Used by Canvas to stamp CanvasFrame::windowOffset at
        /// paint time. Tier 3 Phase 3.4: this is a thin wrapper —
        /// while an AppWindow-driven paint pass is in flight (i.e.
        /// `AppWindow::activeFrameBuilder() != nullptr`), it returns
        /// the FrameBuilder's accumulator top (pushed by the widget
        /// tree walker and the leaf paint code). Otherwise it falls
        /// back to `legacyComputeWindowOffset` which walks the parent
        /// chain summing positions and subtracting any ancestor
        /// scroll contributions.
        Composition::Point2D computeWindowOffset() const;

        /// Tier 3 Phase 3.4 scaffolding. The pre-Phase-3.4
        /// implementation of `computeWindowOffset`, exposed for
        /// callers that explicitly want the parent-chain walk
        /// (today: the `computeWindowOffset` wrapper itself when no
        /// FrameBuilder is in flight, and `FrameBuilder::ScopedViewOffset`
        /// computing the absolute to push). Disappears in Tier 4 once
        /// the accumulator is the only path.
        Composition::Point2D legacyComputeWindowOffset() const;

        /// Returns the scroll offset that this View contributes to
        /// child content positioning. Non-scrolling Views return {0,0}.
        /// ScrollView overrides this to return its scrollOffset.
        ///
        /// Tier 3 Phase 3.6: superseded by `contentOffset()` for the
        /// FrameBuilder accumulator path. Kept as a public method for
        /// callers outside the engine; the engine's offset walk now
        /// reads `contentOffset()`. Removed entirely in Tier 4.
        virtual Composition::Point2D scrollOffsetContribution() const;

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

        /// Called by Widget::executePaint after onPaint. A no-op since
        /// Phase 3.8 collapsed per-view canvases: UIView / SVGView submit
        /// their DisplayList through the window-level FrameBuilder during
        /// paint, so there is no per-view frame left to send here.
        virtual void submitPaintFrame(int submissions) { (void)submissions; }

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
