
#include "omegaWTK/UI/Widget.h"
#include "omegaWTK/UI/LayoutManager.h"   // Phase 4.5: ContainerInsets / ContainerOverflowMode / ContainerClampPolicy moved here.
#include "omegaWTK/Core/Core.h"
#include <cstddef>
#include <cstdint>

#ifndef OMEGAWTK_WIDGETS_BASICWIDGETS_H
#define OMEGAWTK_WIDGETS_BASICWIDGETS_H


namespace OmegaWTK {

// Phase 4.5: `ContainerInsets`, `ContainerOverflowMode`, and
// `ContainerClampPolicy` moved to `LayoutManager.h` where they
// belong (they describe layout policy, not widget shape). The
// names are still in `OmegaWTK::`, so existing call sites that
// just include `BasicWidgets.h` continue to work via the include
// above. No `using` aliases needed.

   /**
 * @brief A widget designed for holding other widgets (No rendering or native event handling can change Widget positioning)
 * 
 */
class OMEGAWTK_EXPORT Container: public Widget {
    // Phase 4.5: child-rect clamping is owned by `ContainerLayout` set
    // on the backing `View` as its `LayoutManager`. The policy + the
    // last-stable-bounds cache moved into the manager; `Container`
    // keeps only a forwarder for backward compatibility on
    // `setClampPolicy` / `getClampPolicy`.
    ContainerLayout containerLayout_ {};
    // Whether the parent layout may resize / cross-stretch this container
    // (see `setResizeWithParent`). Default true preserves the historical
    // "containers grow with their parent" behavior.
    bool resizeWithParent_ = true;
protected:
    OmegaCommon::Vector<WidgetPtr> children;
    bool layoutPending = true;
    // Phase 4.5: `inLayout` re-entry guard kept for `StackWidget`'s
    // bespoke layoutChildren (until Phase 4.6 replaces it with
    // `FlexLayout`). Container's own relayout/clampChild path no
    // longer needs it — `ContainerLayout::arrange` is one-shot per
    // call — but the field remains protected for subclasses.
    bool inLayout = false;

    void wireChild(const WidgetPtr & child);
    void unwireChild(const WidgetPtr & child);

    void onThemeSet(Native::ThemeDesc & desc) override;

    // Phase 4.5: virtual hook kept ONLY so `StackWidget::layoutChildren`
    // can override (its bespoke flexbox lives behind this method until
    // Phase 4.6 replaces it with `FlexLayout`). `Container`'s own
    // relayout/clamp path no longer calls this — it drives
    // `ContainerLayout::arrange` directly.
    virtual void layoutChildren() {}

    void onMount() override;
    void resize(Composition::Rect & newRect) override;

    // Phase 4.5: `clampChildRect` / `onChildRectCommitted` route through
    // the `ContainerLayout` policy now. `clampChildRect` stays as a
    // virtual override so `Widget`'s commit pipeline can still ask the
    // container "is this rect OK?", and `onChildRectCommitted` keeps
    // its hook for child-driven resize requests (the deferred
    // `requestFrame` defers the relayout into the next frame).
    Composition::Rect clampChildRect(const Widget & child,const GeometryProposal & proposal) const override;
    void onChildRectCommitted(const Widget & child,
                              const Composition::Rect & oldRect,
                              const Composition::Rect & newRect,
                              GeometryChangeReason reason) override;
public:
    // WIDGET_CONSTRUCTOR()
    explicit Container(Composition::Rect rect);
    explicit Container(ViewPtr view);

    void setClampPolicy(const ContainerClampPolicy & policy);
    const ContainerClampPolicy & getClampPolicy() const;

    std::size_t childCount() const;
    Widget *childAt(std::size_t idx) const;
    virtual WidgetPtr addChild(const WidgetPtr & child);
    virtual bool removeChild(const WidgetPtr & child);
    OmegaCommon::ArrayRef<WidgetPtr> childWidgets() override;

    /// Layout containers are the one widget family the layout may resize:
    /// a Stack (and future Grid / Table / Tree) must grow and shrink with
    /// its parent to redistribute space to its children. Leaves stay
    /// frozen (see Widget::isLayoutResizable). Input widgets that are not
    /// layout containers (TextInput, Slider) inherit Widget directly, so
    /// they stay frozen without overriding this back.
    ///
    /// Default `true` preserves that behavior. `setResizeWithParent(false)`
    /// pins the container to its own size: the parent layout neither
    /// flex-resizes it nor stretches it across the cross axis (the case for
    /// a fixed-size panel, or a nested scroll container that must not be
    /// widened to a scrollable page). Both the main-axis resize
    /// (`isLayoutResizable`) and the cross-axis Stretch override
    /// (`layoutCrossStretchAllowed`) honor the flag.
    bool isLayoutResizable() const override { return resizeWithParent_; }
    bool layoutCrossStretchAllowed() const override { return resizeWithParent_; }
    void setResizeWithParent(bool resizeWithParent);
    bool resizeWithParent() const { return resizeWithParent_; }

    void relayout();

    ~Container() override;

};

/**
 * @brief Construction options for `ScrollableContainer`.
 *
 * `verticalScroll` / `horizontalScroll` select which axes accept
 * scroll-wheel input and draw a scroll bar. `autoSizeContent` is the
 * S2 auto-extent toggle: when `true` the content host grows to the
 * bounding box of its children; when `false` the caller owns the
 * extent via `setContentSize`. S1 only honors the explicit-size path
 * (the host never auto-grows yet), so the flag is stored but the
 * union-of-rects logic lands in S2.
 */
struct OMEGAWTK_EXPORT ScrollableContainerOptions {
    bool verticalScroll = true;
    bool horizontalScroll = false;
    bool autoSizeContent = true;

    /// Whether the parent layout may resize / cross-stretch the scroll
    /// viewport (mirrors `Container::setResizeWithParent`). Default true
    /// keeps the historical behavior — a ScrollableContainer in a
    /// Stretch-aligned stack fills the cross axis. Set false for a fixed
    /// viewport, e.g. a nested scroll container inside a scrollable page
    /// that must keep its own width rather than be stretched to the page.
    bool resizeWithParent = true;
};

/**
 * @brief A container that allows its content to overflow, and can be scrolled.
 *
 * Two-view composite (ScrollableContainer-Implementation-Plan §3 Option
 * A): the widget's root view is a `ScrollView` (offset + clip +
 * scroll-wheel routing) whose single child is a content `View`. A
 * private inner `Container` wraps that same content view and owns the
 * child widgets, so `addChild` / `removeChild` / `childWidgets` forward
 * to it and the rest of the framework (theme, layout, host walks) sees
 * the children at the right level of nesting through `childWidgets()`.
 * The content view's rect is the scroll extent and may exceed the
 * viewport; the `ScrollView` clips to the viewport and shifts
 * descendants by its scroll offset.
 */
class OMEGAWTK_EXPORT ScrollableContainer : public Widget {
    ScrollableContainerOptions options_;
    // The content host view — shared with both the root `ScrollView`
    // (as its scrolled child) and `contentWidget_` (as its backing
    // view). Its rect is the content extent.
    ViewPtr contentView_;
    // Implementation-private inner Container — not exposed because
    // callers should not stack multiple containers inside the same
    // scroll viewport. addChild / removeChild / childWidgets forward
    // to it.
    SharedHandle<Container> contentWidget_;

    // The `ScrollView` + content `View` must exist before the base
    // `Widget(ViewPtr)` constructor runs, so they are built by this
    // static helper and threaded in through a delegating constructor.
    // Both are typed as `ViewPtr` to keep `ScrollView` out of this
    // header; `scrollView` is really a `ScrollView` (upcast).
    struct Composite {
        ViewPtr scrollView;
        ViewPtr contentView;
    };
    static Composite BuildComposite(const Composition::Rect & rect,
                                    const ScrollableContainerOptions & options);
    ScrollableContainer(Composite composite,
                        const ScrollableContainerOptions & options);
protected:
    // No-op like Container's: the framework's onThemeSetRecurse walk
    // applies the theme to the scrolled children through childWidgets().
    void onThemeSet(Native::ThemeDesc & desc) override;
public:
    explicit ScrollableContainer(Composition::Rect rect,
                                 const ScrollableContainerOptions & options = {});

    // Forwarded to contentWidget_. Signatures match Container's so a
    // caller can swap a Container for a ScrollableContainer with no
    // code changes.
    WidgetPtr addChild(const WidgetPtr & child);
    bool removeChild(const WidgetPtr & child);
    OmegaCommon::ArrayRef<WidgetPtr> childWidgets() override;

    // Explicit content sizing. S1 ships this directly; S2 adds the
    // auto-sizing path gated on `options_.autoSizeContent`.
    void setContentSize(float w, float h);
    Composition::Point2D contentSize() const;

    const ScrollableContainerOptions & options() const { return options_; }

    // Honor `options_.resizeWithParent` so a scroll viewport can be pinned
    // to its own size (not flex-resized, not cross-stretched) by the parent
    // layout — the nested-scroll-on-a-scrollable-page case.
    bool isLayoutResizable() const override { return options_.resizeWithParent; }
    bool layoutCrossStretchAllowed() const override { return options_.resizeWithParent; }

    ~ScrollableContainer() override;
};


}
#endif //OMEGAWTK_WIDGETS_BASICWIDGETS_H
