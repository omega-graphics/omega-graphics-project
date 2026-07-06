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
    // Forward decl — `View::isAnimating` takes the scheduler by reference;
    // its private header lives under `src/UI/` and must not leak into this
    // public header.
    class AnimationScheduler;
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
        // §2.3a C1: opaque-enum forward declaration (fixed underlying
        // type, so this is a complete type for by-value use) — lets
        // `View::setCursorShape` name it without View.h pulling in the
        // heavy `NativeWindow.h` (and its X11 `CursorShape` macro dance).
        // The full definition lives in NativeWindow.h; ViewImpl.h /
        // View.Core.cpp include it where the enumerators are needed.
        enum class CursorShape : int;
    }


    class Container;
    class Widget;
    class ViewDelegate;
    class ScrollView;
    // §2.3a F2: `View::focus`/`blur` route through the owning host's
    // FocusManager; the host pointer propagates down the View tree via
    // `View::setTreeHostRecurse`. Both are used only by pointer here.
    class WidgetTreeHost;
    class FocusManager;
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



    /// §2.3a (Focus, F1): why focus last changed on a View. Recorded by
    /// `View::focus(reason)` (and, from F2 on, the FocusManager) and read
    /// back via `View::lastFocusReason()`. The reason gates focus-ring
    /// rendering — keyboard-driven reasons (Tab / Backtab / Shortcut /
    /// Popup / ActiveWindow) show the ring; Mouse / Other suppress it
    /// (see §2.3a "Focus ring visibility"). Defined as a free enum (not
    /// nested in `View`) so `View::focus`'s default argument and the
    /// later `FocusManager` signatures can both name it without a
    /// `View::` qualifier. Imported verbatim from Qt's `Qt::FocusReason`,
    /// which is the conventional vocabulary for this gate.
    enum class FocusReason : std::uint8_t {
        Mouse,          // ClickFocus path; ring usually suppressed
        Tab,            // FocusManager::focusNext
        Backtab,        // FocusManager::focusPrevious
        Shortcut,       // hotkey landed on this view
        ActiveWindow,   // window became key; restored focus
        Popup,          // owner of a popup got focus back when popup closed
        Restore,        // explicit clearFocus() returned focus to prior holder
        Other           // programmatic View::focus() with no reason
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
        /// §2.3a F2: propagate the owning `WidgetTreeHost` down this
        /// View subtree (mirrors `setSyncLaneRecurse`). Called by
        /// `Widget::setTreeHostRecurse` on attach/detach and by
        /// `addSubView` so a late-added subview inherits its window's
        /// host. The stored pointer is what `View::focus`/`blur` consult
        /// to reach `host->focusManager()`; a detached view (null host)
        /// makes those calls no-ops.
        void setTreeHostRecurse(WidgetTreeHost *host);
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
        /// §2.3a F2: the FocusManager owns the per-View `focused_` flag
        /// and writes it (plus `lastReason_`) directly through `impl_`
        /// in `setFocus`/`clearFocus`.
        friend class FocusManager;
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

        /// ScrollView-4.7-Integration-Plan V2: deliver a native event to
        /// this view, then bubble it up the parent chain until a handler
        /// consumes it (sets `event->handled`). `emit` (inherited) hits
        /// only this view's own receivers; `dispatchEvent` is the
        /// bubbling wrapper the WidgetTreeHost uses so an event that lands
        /// on a deep child (e.g. the wheel over a leaf inside a
        /// ScrollView) reaches an ancestor handler. Dispatch starts here
        /// (the deepest hit) and walks toward the root, so the innermost
        /// capable handler consumes first.
        void dispatchEvent(Native::NativeEventPtr event);

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
        /// ScrollView-4.7-Integration-Plan V2.1: mark this view dirty for
        /// Paint and ask the owning host to schedule a frame. Unlike the
        /// idle-context callers that batch and call `AppWindow::refresh()`
        /// once, this is for a view-internal change driven *inside* a
        /// native-event handler (e.g. a ScrollView updating its offset on
        /// a wheel event) where no surrounding code will request the
        /// frame. `requestFrame` coalesces, so a burst of these collapses
        /// to one paint. A no-op (markDirty only) when the view is not yet
        /// attached to a host. The whole-tree Paint walk
        /// (FrameBuilder.cpp — "no subtree pruning at the node level")
        /// then re-folds the new `contentOffset()` into every descendant.
        void scheduleRepaint();
        /// UIView-Render-Redesign Phase G.3.3: same dirty-bit marking
        /// and ancestor `descendantDirty` propagation as `markDirty`,
        /// but DOES NOT bump `contentVersion()`. Used by the window
        /// resize repaint path (`WidgetTreeHost::forceFullRepaint`):
        /// every node needs its Style / Layout / Paint passes to re-run
        /// so the relayout settles and the tree re-paints at the new
        /// window resolution, but a resize does not by itself change a
        /// View's *own* painted content — a View whose pixel size is
        /// unchanged draws exactly the same thing. The per-View content
        /// cache keys on `(nodeId, contentVersion, sizeBucket, scale)`,
        /// so the cache's size bucket already invalidates exactly the
        /// Views whose size changed (miss -> recapture) while
        /// size-unchanged Views hit and re-blit at their new position.
        /// Bumping `contentVersion` here would defeat that — it would
        /// force a cache miss on every View on every resize tick. Use
        /// `markDirty` (which bumps) for genuine content changes; use
        /// this for resize/relayout-driven repaints.
        void markDirtyNoContentBump(uint8_t bits);
        /// Current self-only dirty mask (combination of DirtyBit
        /// values). Excludes the propagated descendant mask.
        uint8_t dirtyBits() const;
        /// Phase 4.7.3: OR of every descendant's `dirtyBits()`.
        /// Maintained incrementally by `markDirty()`; cleared
        /// together with `dirtyBits_` by `clearDirtyBits()`.
        uint8_t descendantDirty() const;
        /// Reset BOTH the self mask AND the propagated descendant
        /// mask to zero (Phase 4.7.3 — pre-4.7.3 this cleared only
        /// the self mask). Does NOT touch `contentVersion()` —
        /// that's a monotonic generation counter (G.3.0), not a flag.
        void clearDirtyBits();
        /// UIView-Render-Redesign Phase G.3.0 (semantic set by
        /// G.3.2-rev2): monotonic per-View content-generation counter.
        /// Any `markDirty(bits)` increments it (every dirty bit triggers
        /// a repaint that can change this View's output, including a
        /// Style-only hover). It reflects only this View's OWN content —
        /// the increment does not propagate to ancestors.
        /// `clearDirtyBits()` does NOT reset it. The per-View content
        /// cache keys on `(nodeId, contentVersion, sizeBucket, scale)`,
        /// so a hover that changes one View re-captures only that View
        /// while siblings blit from cache.
        std::uint64_t contentVersion() const;
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

        /// Resize-Clamping Plan §1.7: content-driven sizing. A widget whose
        /// intrinsic size depends on the space it is given — a wrapping
        /// `Label` whose height is a function of its width — installs this.
        /// Given an available size (dp) it writes the widget's intrinsic dp
        /// size. The parent's `FlexLayout::measure` consults it so such a
        /// widget is sized to its content instead of a frozen constant; left
        /// unset, a widget is treated as fixed-size (the parent uses its
        /// rect). This is the layout's sanctioned exception to the §1.5
        /// freeze: a widget's geometry may change on resize when its own
        /// content requires it (text reflow), never because the layout
        /// arbitrarily squashes it.
        using ContentMeasureFn =
            std::function<void(float availWidthDp, float availHeightDp,
                               float & outWidthDp, float & outHeightDp)>;
        void setContentMeasure(ContentMeasureFn fn);
        bool hasContentMeasure() const;
        /// Run the content-measure hook. Falls back to the current rect (dp)
        /// when no hook is installed, so callers need not branch.
        void measureContent(float availWidthDp, float availHeightDp,
                            float & outWidthDp, float & outHeightDp) const;

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

        /// Widget-View-Paint-Lifecycle-Plan Tier D / D7.4 (2026-06-04):
        /// `:state(name)` custom pseudo-class API. The open-ended
        /// counterpart to the enumerated `pseudoClassBits` surface:
        /// widget / app code names states (`loading`, `selected`,
        /// `error`, `expanded`, …) and flips them on a view. The
        /// `StyleSheets::StyleResolver` matches selector `customStates`
        /// entries against this set during cascade resolution. The
        /// state set is independent of `pseudoClassBits`; a view can
        /// carry both at once. `setState`/`clearState` call
        /// `markDirty(Style)` only when the set actually changes — a
        /// no-op flip (set when already on, clear when already off)
        /// does not invalidate the cascade.
        void setState(const OmegaCommon::String & name);
        void clearState(const OmegaCommon::String & name);
        /// Convenience: write either side of the flip in one call.
        void setState(const OmegaCommon::String & name, bool on);
        bool hasState(const OmegaCommon::String & name) const;

        /// §2.3a F1: per-view focusability declaration — a bitmask of
        /// the ways this view may receive keyboard focus.
        ///   NoFocus     — never focusable (default; shape primitives,
        ///                  Label, and every view that does not opt in).
        ///   ClickFocus  — focusable by direct mouse click + View::focus().
        ///   TabFocus    — focusable by keyboard traversal (Tab / Shift-Tab).
        ///   StrongFocus — ClickFocus | TabFocus. Default for interactive
        ///                  widgets (Button, TextInput, Checkbox).
        ///   WheelFocus  — StrongFocus plus focus-on-scroll-wheel. Default
        ///                  for editable scrollable surfaces (TextArea).
        /// A scoped enum with `|`/`&` operator overloads (defined below
        /// the class) so `policy & FocusPolicy::TabFocus` reads cleanly.
        /// Default `NoFocus` preserves pre-F1 behavior: nothing is
        /// focusable until a widget opts in via `setFocusPolicy`.
        enum class FocusPolicy : std::uint8_t {
            NoFocus     = 0,
            ClickFocus  = 1 << 0,
            TabFocus    = 1 << 1,
            StrongFocus = ClickFocus | TabFocus,
            WheelFocus  = StrongFocus | (1 << 2)
        };

        void setFocusPolicy(FocusPolicy policy);
        FocusPolicy focusPolicy() const;

        /// Convenience predicates derived from `focusPolicy()`.
        bool isFocusable() const;       // policy != NoFocus
        bool isClickFocusable() const;  // policy carries the ClickFocus bit
        bool isTabFocusable() const;    // policy carries the TabFocus bit

        /// True iff the host's FocusManager has selected this view. The
        /// flag is owned by the FocusManager (lands in F2); at F1 it is
        /// always false because no manager exists to set it.
        bool isFocused() const;

        /// Request / drop keyboard focus. **F1 ships these as stubs:**
        /// `focus(reason)` records `reason` (later readable via
        /// `lastFocusReason()`) but cannot actually select the view
        /// because the FocusManager does not exist yet; `blur()` is a
        /// no-op. F2 retrofits both to route through the host's
        /// FocusManager (`treeHost->focusManager()`), where a detached
        /// view's `focus()` stays a no-op. The `reason` is what decides
        /// whether a focus ring renders later (see `FocusReason`).
        void focus(FocusReason reason = FocusReason::Other);
        void blur();

        /// Why focus last changed on this view. Read by a widget's
        /// `rebuildStyle()` hook (F2+) to gate focus-ring rendering.
        FocusReason lastFocusReason() const;

        /// §2.3a C1: declarative per-view cursor shape — "while the cursor
        /// is over me, show this." Like focus and (later) tooltips, the
        /// View only *declares* the shape; it never touches the OS cursor.
        /// The virtual hover dispatcher in `WidgetTreeHost` resolves the
        /// topmost hovered view's effective shape — walking up to the
        /// nearest ancestor that set one (CSS / Qt cursor inheritance) —
        /// and commits it to the single per-window OS cursor sink
        /// (`NativeWindow::setCursorShape`, via `AppWindow`).
        ///
        /// A view with no shape set (the default) is transparent to the
        /// walk: its subtree inherits the nearest ancestor's shape, and
        /// `CursorShape::Arrow` is the ultimate fallback when nothing in
        /// the chain set one. Calling `setCursorShape` makes this view
        /// authoritative for its subtree's cursor. The getter returns the
        /// view's own declared shape, or `Arrow` when unset (it does *not*
        /// resolve up the ancestor chain — that resolution is the
        /// dispatcher's job at hover time).
        void setCursorShape(Native::CursorShape shape);
        Native::CursorShape cursorShape() const;

        /// Phase 4.7.0: the polymorphic Paint-pass hook. Per-node:
        /// emits THIS view's draw ops into `pc.displayList` and reads
        /// `pc.offset` for absolute window positioning. Does NOT recurse
        /// — subtree traversal lives in `FrameBuilder::buildFrame`'s
        /// walker (4.7.1), which calls this once per node. The default
        /// (base `View`) is a pass-through no-op; `UIView`, `SVGView`,
        /// `ScrollView` override to emit their ops.
        virtual void paint(Composition::PaintContext & pc);

        /// ScrollView-4.7-Integration-Plan V3: the post-order companion to
        /// `paint`. The FrameBuilder paint walker calls this AFTER all of
        /// this node's subviews have painted, with `pc.offset` restored to
        /// this node's own absolute offset. It is the place to emit ops
        /// that must bracket the children — `ScrollView` emits its
        /// `PopClip` here to close the `PushClip` its `paint` opened.
        /// Default is a no-op.
        virtual void paintAfterChildren(Composition::PaintContext & pc);

        /// ScrollView-4.7-Integration-Plan V3: when true, this view clips
        /// its descendants (emits a `PushClip` in `paint` + `PopClip` in
        /// `paintAfterChildren`), and its whole subtree must paint live so
        /// the clip bracket is never split across content-cache capture
        /// markers (a cache hit would skip the wrapped `PushClip` while the
        /// `PopClip` still ran → scissor-stack imbalance). The cache walker
        /// recurses such a subtree through the non-cache path. Default
        /// false; `ScrollView` overrides to true.
        virtual bool clipsContentSubtree() const;

        /// The margin (logical px, per side) by which this view's paint
        /// extends *beyond* its layout rect. Driven today by a resolved
        /// drop shadow (the shadow quad reaches `max(2, blur+2) + |offset|`
        /// past the shape on each side). The per-View content cache
        /// (UIView-Render-Redesign-Plan §G.3.4) inflates its capture region
        /// by this so bleeding effects are not scissored to the layout rect
        /// on capture. Default (base `View`) is zero; `UIView` overrides.
        /// Extension point for any future bleeding effect (outer glow, …).
        struct PaintBleed { float left = 0.f, top = 0.f, right = 0.f, bottom = 0.f; };
        virtual PaintBleed paintBleed();

        /// Whether any animation owned by this view is currently active in
        /// `scheduler`. The per-View content cache (UIView-Render-Redesign-
        /// Plan §G.3.2 eligibility rule #1) skips caching an animating view
        /// so its tween frames render live instead of blitting a stale
        /// captured texture. Base `View` checks only its own node id;
        /// `UIView` overrides to ALSO check its per-element node ids —
        /// element-level animations (drop shadow, per-element style
        /// transitions) register under `(elementNodeId, …)`, NOT the view
        /// node id, so the bare `scheduler.hasAnyAnimationFor(nodeId())`
        /// check misses them and the view freezes on its start frame.
        virtual bool isAnimating(const AnimationScheduler & scheduler) const;

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

    /// §2.3a F1: bitmask operators for the scoped `View::FocusPolicy`
    /// enum. `enum class` does not synthesize these, but the policy is a
    /// bitmask (`StrongFocus == ClickFocus | TabFocus`), so combining and
    /// testing bits — `policy & FocusPolicy::TabFocus` — must read
    /// cleanly. Free `inline` functions in the `OmegaWTK` namespace,
    /// reached via ADL on the enum's enclosing class/namespace.
    inline View::FocusPolicy operator|(View::FocusPolicy a, View::FocusPolicy b){
        return static_cast<View::FocusPolicy>(
            static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
    }
    inline View::FocusPolicy operator&(View::FocusPolicy a, View::FocusPolicy b){
        return static_cast<View::FocusPolicy>(
            static_cast<std::uint8_t>(a) & static_cast<std::uint8_t>(b));
    }

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
