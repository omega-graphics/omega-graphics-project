#ifndef OMEGAWTK_UI_LAYOUTMANAGER_H
#define OMEGAWTK_UI_LAYOUTMANAGER_H

#include "omegaWTK/Core/Core.h"
#include "omegaWTK/Composition/Geometry.h"
#include "View.h"   // ResizeClamp / ChildResizePolicy / ChildResizeSpec (Tier-1 types, kept public).

#include <cstdint>
#include <unordered_map>

namespace OmegaWTK {

// UIView-Render-Redesign-Plan Tier 4 Phase 4.5: parent-owned layout
// strategy. The `LayoutManager` interface arranges a node's **child
// nodes** (child `View`s / `Widget`s); it does NOT touch the
// intra-`UIView` element layout (which stays inside `UIView`'s own
// `arrange()` / `paint()` against `UIViewLayoutV2`).
//
// Phase 4.5 ships four reusable public built-ins:
// `AbsoluteLayout` (the default — child positioned by its own rect,
// per-child clamp via FitContent semantics), `FillLayout` (every
// child stretched to the parent's content rect), `StackLayout` (H/V
// no-flex sequential placement — Phase 4.6's `FlexLayout` is the
// load-bearing one for `StackWidget`), and `ContainerLayout` (the
// lifted version of `Container::clampChildRect`, parameterized by
// `ContainerClampPolicy`).
//
// Precedent (recorded 2026-05-21): LayoutManagers are reusable
// first-class built-ins, one per container *kind* — never private
// to a widget. The family mirrors the container-widget family
// (`StackWidget` ↔ `FlexLayout`; future `Grid` ↔ `GridLayout`,
// `Table` ↔ `TableLayout`, `Tree` ↔ `TreeLayout`). So a new
// container kind = "add a `LayoutManager` and set it as the backing
// View's manager"; never "write another `layoutChildren()`."
//
// Invocation in 4.5: existing entry points (the resize handler,
// `Container::relayout`, `runWidgetLayout`, `WidgetTreeHost`'s
// resize session) call `parent->layoutManager()->arrange(parent,
// parent->getRect())`. Phase 4.7 hoists invocation into
// `FrameBuilder`'s Measure / Arrange passes; 4.5 only swaps the
// layout *math*, additive-then-centralize.

// ---------------------------------------------------------------------------
// ContainerLayout policy types (moved 2026-05-31 from BasicWidgets.h —
// these describe layout policy, not widget shape, so they belong with
// the layout manager that consumes them. BasicWidgets.h re-exports for
// source compatibility.)
// ---------------------------------------------------------------------------

enum class ContainerOverflowMode : std::uint8_t {
    Clamp,
    Allow
};

struct OMEGAWTK_EXPORT ContainerInsets {
    float left   = 0.f;
    float top    = 0.f;
    float right  = 0.f;
    float bottom = 0.f;
};

struct OMEGAWTK_EXPORT ContainerClampPolicy {
    bool                   clampPositionToBounds            = true;
    bool                   clampSizeToBounds                = true;
    bool                   enforceMinSize                   = true;
    float                  minWidth                         = 1.f;
    float                  minHeight                        = 1.f;
    ContainerInsets        contentInsets                    {};
    ContainerOverflowMode  horizontalOverflow               = ContainerOverflowMode::Clamp;
    ContainerOverflowMode  verticalOverflow                 = ContainerOverflowMode::Clamp;
    bool                   keepLastStableBoundsOnInvalidResize = true;
};

// ---------------------------------------------------------------------------
// Stack axis (used by StackLayout in 4.5; by FlexLayout in 4.6)
// ---------------------------------------------------------------------------

enum class LayoutAxis : std::uint8_t {
    Horizontal,
    Vertical
};

// ---------------------------------------------------------------------------
// LayoutManager — the abstract interface
// ---------------------------------------------------------------------------

struct LayoutSize {
    float w = 0.f;
    float h = 0.f;
};

class OMEGAWTK_EXPORT LayoutManager {
public:
    virtual ~LayoutManager() = default;

    /// Measure pass — bottom-up. Returns the parent's desired size
    /// given the available rect. Phase 4.5 stubs: the four built-ins
    /// here all return the available size unchanged (none of them
    /// distribute free space — `FlexLayout` in Phase 4.6 is the first
    /// manager that actually consumes a Measure result). 4.7 makes
    /// `FrameBuilder` run Measure top-down across the dirty subtree
    /// before Arrange.
    virtual LayoutSize measure(View & node, const Composition::Rect & avail) = 0;

    /// Arrange pass — top-down. The manager iterates `node.subviews()`
    /// and assigns each child a final rect (in parent-local coords) by
    /// calling `child->resize(...)`. Layout-affecting math here is
    /// lifted unchanged from the pre-migration `ViewResizeCoordinator`
    /// / `Container::clampChildRect` / `StackWidget::layoutChildren`
    /// codepaths.
    virtual void       arrange(View & node, const Composition::Rect & finalRectLocal) = 0;

    /// Recursive content-minimum: the smallest size this node can take
    /// without shrinking its own content below intrinsic. The default
    /// (leaves and absolute parents) returns the node's current rect — a
    /// frozen leaf's intrinsic size. FlexLayout overrides it to aggregate
    /// children (sum on the main axis, max on the cross axis, plus spacing
    /// / padding / margins). Feeds the per-container min-clamp in arrange
    /// and the window-level setMinSize (Resize-Clamping plan Phase 2).
    virtual LayoutSize minSize(View & node);

    // Phase 4.5: the static clamp helper, lifted from the deleted
    // `ViewResizeCoordinator::clampRectToParent`. Three live callers
    // outside the manager hierarchy: `StackWidget::layoutChildren`
    // (until Phase 4.6 replaces it with `FlexLayout`), `UIView::paint`
    // (intra-element clamp during paint — NOT child layout; stays
    // forever), and the manager built-ins themselves.
    static Composition::Rect clampRectToParent(const Composition::Rect & requested,
                                               const Composition::Rect & parentContentRect,
                                               const ChildResizeSpec & spec);
};

// ---------------------------------------------------------------------------
// Built-in: AbsoluteLayout — the default (current no-explicit-layout
// behavior). Each child keeps its own rect, optionally clamped to the
// parent's bounds via `FitContent` semantics.
// ---------------------------------------------------------------------------

class OMEGAWTK_EXPORT AbsoluteLayout : public LayoutManager {
public:
    /// Process-wide singleton — `View::layoutManager()` returns this
    /// when no manager has been set explicitly. Reused everywhere so
    /// the default path costs no allocation. Stateless; safe to share.
    static AbsoluteLayout & instance();

    LayoutSize measure(View & node, const Composition::Rect & avail) override;
    void       arrange(View & node, const Composition::Rect & finalRectLocal) override;
};

// ---------------------------------------------------------------------------
// Built-in: PassthroughLayout — leaves every child's rect exactly as set;
// `arrange` is a no-op. Unlike `AbsoluteLayout` (which FitContent-clamps an
// oversized child down to the parent box), this manager never resizes or
// repositions a child, so a child may exceed the parent's bounds freely.
// This is what a scroll viewport needs: `ScrollView`'s content child owns an
// extent that is deliberately larger than the viewport, and the host (not
// the viewport) decides that extent and the content's origin. See
// ScrollView-4.7-Integration-Plan §3 V1. Stateless process-wide singleton.
// ---------------------------------------------------------------------------

class OMEGAWTK_EXPORT PassthroughLayout : public LayoutManager {
public:
    /// Process-wide singleton — stateless, safe to share, costs no
    /// allocation (mirrors `AbsoluteLayout::instance()`).
    static PassthroughLayout & instance();

    LayoutSize measure(View & node, const Composition::Rect & avail) override;
    void       arrange(View & node, const Composition::Rect & finalRectLocal) override;
};

// ---------------------------------------------------------------------------
// Built-in: FillLayout — every child stretched to the parent's full
// content rect. Useful for "single-child wrapper" parents (one child
// fills, multi-child = all children overlap at parent extent).
// ---------------------------------------------------------------------------

class OMEGAWTK_EXPORT FillLayout : public LayoutManager {
public:
    LayoutSize measure(View & node, const Composition::Rect & avail) override;
    void       arrange(View & node, const Composition::Rect & finalRectLocal) override;
};

// ---------------------------------------------------------------------------
// Built-in: StackLayout — H or V sequential placement, no flex. Children
// are positioned one after the other along the main axis, each keeping
// its current size (clamped to the parent). Phase 4.6's `FlexLayout` is
// the load-bearing replacement that distributes free space; this one
// covers the "fixed-size children stacked" case for code that does not
// need flex (and stands in for the 4.5 ordering test surface).
// ---------------------------------------------------------------------------

class OMEGAWTK_EXPORT StackLayout : public LayoutManager {
    LayoutAxis axis_  = LayoutAxis::Vertical;
    float      spacing_ = 0.f;
public:
    explicit StackLayout(LayoutAxis axis = LayoutAxis::Vertical, float spacing = 0.f);

    void setAxis(LayoutAxis axis)   { axis_ = axis; }
    void setSpacing(float spacing)  { spacing_ = spacing; }
    LayoutAxis axis() const         { return axis_; }
    float      spacing() const      { return spacing_; }

    LayoutSize measure(View & node, const Composition::Rect & avail) override;
    void       arrange(View & node, const Composition::Rect & finalRectLocal) override;
};

// ---------------------------------------------------------------------------
// Built-in: ContainerLayout — the lifted `Container::clampChildRect`.
// `ContainerClampPolicy` (insets, min/max, overflow modes,
// keep-last-stable-bounds) becomes its constructor / setter parameters.
// `Container` sets one of these as its backing View's LayoutManager
// (Phase 4.5 migration of `Container::layoutChildren` /
// `Container::clampChildRect`).
// ---------------------------------------------------------------------------

class OMEGAWTK_EXPORT ContainerLayout : public LayoutManager {
    ContainerClampPolicy policy_ {};
    // Per-instance "last good content bounds" cache: smooths transient
    // invalid rects during resize, identical to the pre-migration
    // `Container::lastStableContentBounds` field. Mutable because the
    // cache update happens during `arrange`'s read path.
    mutable bool              hasLastStableContentBounds_ = false;
    mutable Composition::Rect lastStableContentBounds_    {Composition::Point2D{0.f,0.f},1.f,1.f};
public:
    explicit ContainerLayout(const ContainerClampPolicy & policy = {});

    void setPolicy(const ContainerClampPolicy & policy);
    const ContainerClampPolicy & policy() const { return policy_; }

    LayoutSize measure(View & node, const Composition::Rect & avail) override;
    void       arrange(View & node, const Composition::Rect & finalRectLocal) override;

    /// Exposed for direct use by `Container::clampChildRect` during the
    /// transitional period — the Widget API still answers a "what would
    /// you clamp this child to" question from `Widget::commitGeometry`.
    /// Once 4.7 centralizes invocation in `FrameBuilder` this can go
    /// private.
    Composition::Rect clampChild(const Composition::Rect & requested,
                                 const Composition::Rect & hostRect) const;
};

// ---------------------------------------------------------------------------
// Built-in: FlexLayout — main-axis flex distribution + cross-axis alignment.
// Phase 4.6: this is the load-bearing manager for `StackWidget` (and any
// future container kind that wants flex). It absorbs the bespoke flexbox
// that lived behind `StackWidget::layoutChildren()` and exposes the same
// algorithm as a reusable public built-in.
//
// Per-child state (`FlexChildSpec`) is stored on the manager and keyed by
// child `View *` — set with `setChildSpec` when the owner (StackWidget,
// etc.) wires a child. Children without an explicit spec get the default
// (`flexGrow = 0`, `flexShrink = 1`, no basis, no min/max, default cross
// alignment). The per-child preferred-size cache lives on the manager too;
// it carries the same "use last-seen good size when the current rect is
// suspicious or a placeholder" semantic that StackWidget's
// `childSizeCache` used pre-migration — Widget rects are unreliable mid-
// resize.
//
// Measure earns its keep here (the four 4.5 built-ins did not need it):
// `measure(node, avail)` collects each child's preferred main / cross size
// into the per-child cache and returns the parent's desired size for the
// available rect; `arrange(node, finalRectLocal)` runs the main-axis
// distribution + cross-axis alignment from that cache and assigns each
// child's final rect via `View::resize`. In 4.6 `arrange()` runs
// `measure()` internally so the existing entry points (resize, relayout)
// continue to work; Phase 4.7 will hoist `measure` to FrameBuilder's
// top-down Measure pass and the cache will be invalidated by
// `DirtyBit::Layout` instead of by spec / child mutations.
// ---------------------------------------------------------------------------

enum class FlexMainAlign : std::uint8_t {
    Start,
    Center,
    End,
    SpaceBetween,
    SpaceAround,
    SpaceEvenly
};

enum class FlexCrossAlign : std::uint8_t {
    Start,
    Center,
    End,
    Stretch
};

struct OMEGAWTK_EXPORT FlexInsets {
    float left   = 0.f;
    float top    = 0.f;
    float right  = 0.f;
    float bottom = 0.f;
};

struct OMEGAWTK_EXPORT FlexOptions {
    LayoutAxis      axis       = LayoutAxis::Vertical;
    float           spacing    = 0.f;
    FlexInsets      padding    {};
    FlexMainAlign   mainAlign  = FlexMainAlign::Start;
    FlexCrossAlign  crossAlign = FlexCrossAlign::Start;
};

struct OMEGAWTK_EXPORT FlexChildSpec {
    /// Owner-side resizable flag (e.g. `Widget::isLayoutResizable()`).
    /// Non-resizable children skip flexGrow / flexShrink / cross-stretch.
    bool                          resizable  = true;
    /// Whether an explicit `FlexCrossAlign::Stretch` from the container is
    /// allowed to widen this child to the cross extent. Default true so a
    /// frozen leaf (e.g. a Separator) still spans the cross axis under a
    /// Stretch directive. A child that owns its own size — a nested
    /// scroll viewport, a fixed-size container — sets this false (via
    /// `Widget::layoutCrossStretchAllowed()`) to keep its intrinsic cross
    /// size even when the parent's crossAlign is Stretch.
    bool                          honorCrossStretch = true;
    float                         flexGrow   = 0.f;
    float                         flexShrink = 1.f;
    Core::Optional<float>         basis      {};
    Core::Optional<float>         minMain    {};
    Core::Optional<float>         maxMain    {};
    Core::Optional<float>         minCross   {};
    Core::Optional<float>         maxCross   {};
    FlexInsets                    margin     {};
    Core::Optional<FlexCrossAlign> alignSelf  {};
};

class OMEGAWTK_EXPORT FlexLayout : public LayoutManager {
    struct ChildEntry {
        FlexChildSpec spec              {};
        float         preferredMain     = 0.f;
        float         preferredCross    = 0.f;
        bool          hasPreferredSize  = false;
    };

    FlexOptions                          options_ {};
    std::unordered_map<View *, ChildEntry> entries_ {};
    bool                                 hasLastStableFrame_ = false;
    Composition::Rect                    lastStableFrame_ {Composition::Point2D{0.f,0.f},1.f,1.f};
public:
    explicit FlexLayout(const FlexOptions & options = {});

    const FlexOptions & options() const { return options_; }
    void                setOptions(const FlexOptions & options);

    /// Set / update the per-child spec keyed by the child's backing
    /// View. Children without an explicit spec use the default. Owners
    /// (StackWidget, etc.) call this in their addChild / setSlot paths.
    void setChildSpec(View * child, const FlexChildSpec & spec);

    /// Drop the spec for `child`. Idempotent — no-op if `child` had no
    /// entry. Owners call this from removeChild.
    void removeChildSpec(View * child);

    /// Return the spec for `child` (or the default if none is set).
    FlexChildSpec childSpec(View * child) const;

    LayoutSize measure(View & node, const Composition::Rect & avail) override;
    void       arrange(View & node, const Composition::Rect & finalRectLocal) override;
    LayoutSize minSize(View & node) override;
};

// ---------------------------------------------------------------------------
// Built-in: GridLayout — uniform-column, content-row grid with span support.
// The load-bearing manager for the `Grid` container widget, completing the
// family the 4.5 precedent named (`StackWidget` ↔ `FlexLayout`; `Grid` ↔
// `GridLayout`). Children are placed row-major (first-fit) into a fixed
// number of equal-width columns; a child may occupy a rectangular block of
// cells via its `GridChildSpec` (columnSpan × rowSpan). Column width is
// uniform — the content width split evenly across the columns, minus the
// inter-column gaps. Row heights are content-driven: each row sizes to the
// tallest single-row child that lands in it, and a multi-row spanner grows
// the last row it covers if its preferred height exceeds the rows it spans.
// `GridCellAlign` positions (Start / Center / End) or fills (Stretch) each
// child within its resolved cell block.
//
// Per-child span state (`GridChildSpec`) is stored on the manager keyed by
// the child's backing `View *`, set via `setChildSpec` when the owning
// `Grid` wires a child (mirrors `FlexLayout::setChildSpec`). Children with
// no explicit spec occupy a single 1×1 cell.
// ---------------------------------------------------------------------------

enum class GridCellAlign : std::uint8_t {
    Start,    ///< Child pinned to the cell's top-left, keeping its own size.
    Center,   ///< Child centered within the cell, keeping its own size.
    End,      ///< Child pinned to the cell's bottom-right, keeping its size.
    Stretch   ///< Child resized to fill the whole cell block.
};

struct OMEGAWTK_EXPORT GridLayoutOptions {
    /// Number of equal-width columns. Clamped to `>= 1` at arrange time.
    unsigned      columns       = 1;
    float         rowSpacing    = 0.f;
    float         columnSpacing = 0.f;
    GridCellAlign cellAlign     = GridCellAlign::Start;
};

struct OMEGAWTK_EXPORT GridChildSpec {
    /// Columns this child occupies. Clamped to `[1, columns]` at arrange.
    unsigned columnSpan = 1;
    /// Rows this child occupies. Clamped to `>= 1` at arrange.
    unsigned rowSpan    = 1;
};

class OMEGAWTK_EXPORT GridLayout : public LayoutManager {
    GridLayoutOptions                         options_ {};
    std::unordered_map<View *, GridChildSpec> specs_   {};
public:
    explicit GridLayout(const GridLayoutOptions & options = {});

    const GridLayoutOptions & options() const { return options_; }
    void                      setOptions(const GridLayoutOptions & options);

    /// Set / update the per-child span spec keyed by the child's backing
    /// View. Children without a spec occupy a single 1×1 cell. The owning
    /// `Grid` calls this from its addChild / setSlot paths.
    void          setChildSpec(View * child, const GridChildSpec & spec);
    /// Drop the spec for `child`. Idempotent. Called from `Grid::removeChild`.
    void          removeChildSpec(View * child);
    /// Return the spec for `child` (or the default 1×1 if none is set).
    GridChildSpec childSpec(View * child) const;

    LayoutSize measure(View & node, const Composition::Rect & avail) override;
    void       arrange(View & node, const Composition::Rect & finalRectLocal) override;
};

} // namespace OmegaWTK

#endif
