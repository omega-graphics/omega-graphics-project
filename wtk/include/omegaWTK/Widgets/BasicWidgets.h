
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
    bool isLayoutResizable() const override { return true; }

    void relayout();

    ~Container() override;

};

/**
 * @brief A container that allows its content to overflow, and can be scrolled.
 * 
 */ 
class OMEGAWTK_EXPORT ScrollableContainer : public Widget {
    WIDGET_CONSTRUCTOR()
};


}
#endif //OMEGAWTK_WIDGETS_BASICWIDGETS_H
