
#include "omegaWTK/UI/Widget.h"
#include "omegaWTK/Core/Core.h"
#include <cstddef>
#include <cstdint>

#ifndef OMEGAWTK_WIDGETS_BASICWIDGETS_H
#define OMEGAWTK_WIDGETS_BASICWIDGETS_H


namespace OmegaWTK {

enum class ContainerOverflowMode : std::uint8_t {
    Clamp,
    Allow
};

struct OMEGAWTK_EXPORT ContainerInsets {
    float left = 0.f;
    float top = 0.f;
    float right = 0.f;
    float bottom = 0.f;
};

struct OMEGAWTK_EXPORT ContainerClampPolicy {
    bool clampPositionToBounds = true;
    bool clampSizeToBounds = true;
    bool enforceMinSize = true;
    float minWidth = 1.f;
    float minHeight = 1.f;
    ContainerInsets contentInsets {};
    ContainerOverflowMode horizontalOverflow = ContainerOverflowMode::Clamp;
    ContainerOverflowMode verticalOverflow = ContainerOverflowMode::Clamp;
    bool keepLastStableBoundsOnInvalidResize = true;
};

   /**
 * @brief A widget designed for holding other widgets (No rendering or native event handling can change Widget positioning)
 * 
 */
class OMEGAWTK_EXPORT Container: public Widget {
    ContainerClampPolicy clampPolicy {};
    mutable bool hasLastStableContentBounds = false;
    mutable Core::Rect lastStableContentBounds {Core::Position{0.f,0.f},1.f,1.f};
protected:
    OmegaCommon::Vector<Widget *> children;
    bool layoutPending = true;
    bool inLayout = false;

    void wireChild(Widget *child);
    void unwireChild(Widget *child);

    void onThemeSet(Native::ThemeDesc & desc) override;
    virtual void layoutChildren();

    void onMount() override;
    void onPaint(PaintReason reason) override;
    void resize(Core::Rect & newRect) override;

    Core::Rect clampChildRect(const Widget & child,const GeometryProposal & proposal) const override;
    void onChildRectCommitted(const Widget & child,
                              const Core::Rect & oldRect,
                              const Core::Rect & newRect,
                              GeometryChangeReason reason) override;
public:
    // WIDGET_CONSTRUCTOR()
    explicit Container(Core::Rect rect);
    explicit Container(ViewPtr view);

    void setClampPolicy(const ContainerClampPolicy & policy);
    const ContainerClampPolicy & getClampPolicy() const;

    std::size_t childCount() const;
    Widget *childAt(std::size_t idx) const;
    virtual WidgetPtr addChild(const WidgetPtr & child);
    virtual bool removeChild(const WidgetPtr & child);
    OmegaCommon::Vector<Widget *> childWidgets() const override;

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
