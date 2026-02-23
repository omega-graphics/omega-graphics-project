
#include "omegaWTK/UI/Widget.h"
#include "omegaWTK/Core/Core.h"
#include <cstddef>
#include <cstdint>

#ifndef OMEGAWTK_WIDGETS_BASICWIDGETS_H
#define OMEGAWTK_WIDGETS_BASICWIDGETS_H

/**
* Every Widget Constructor comes with two default parameters: The rect, and the parent widget.
*/
#define WIDGET_CONSTRUCTOR(args...) static SharedHandle<Widget> Create(const Core::Rect & rect,WidgetPtr parent,...args);
#define WIDGET_CONSTRUCTOR_IMPL(args...) Create(const Core::Rect & rect,WidgetPtr parent,...args)
#define WIDGET_CREATE make

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


// /**
//  * @brief A single view widget responsible for managing one view's capability.
//  * 
//  */

// class OMEGAWTK_EXPORT WrapperWidget : public Widget {
// public:
//     static SharedHandle<WrapperWidget> CreateVideoViewWrapper(const Core::Rect & rect,WidgetPtr parent);
//     static SharedHandle<WrapperWidget> CreateSVGViewWrapper(const Core::Rect & rect,WidgetPtr parent);
//     static SharedHandle<WrapperWidget> CreateUIViewWrapper(const Core::Rect & rect,WidgetPtr parent);
//     static SharedHandle<WrapperWidget> CreateScollViewWrapper(const Core::Rect & rect,WidgetPtr parent);

//     SharedHandle<View> getUnderlyingView();
// };

   /**
 * @brief A widget designed for holding other widgets (No rendering or native event handling can change Widget positioning)
 * 
 */
class OMEGAWTK_EXPORT Container: public Widget {
    OmegaCommon::Vector<Widget *> containerChildren;
    bool layoutPending = true;
    bool inLayout = false;
    ContainerClampPolicy clampPolicy {};
    mutable bool hasLastStableContentBounds = false;
    mutable Core::Rect lastStableContentBounds {Core::Position{0.f,0.f},1.f,1.f};
protected:
    void onThemeSet(Native::ThemeDesc & desc) override;
    virtual void layoutChildren();

    void onMount() override;
    void onPaint(PaintContext & context,PaintReason reason) override;
    void resize(Core::Rect & newRect) override;

    bool acceptsChildWidget(const Widget *child) const override;
    void onChildAttached(Widget *child) override;
    void onChildDetached(Widget *child) override;
    Core::Rect clampChildRect(const Widget & child,const GeometryProposal & proposal) const override;
    void onChildRectCommitted(const Widget & child,
                              const Core::Rect & oldRect,
                              const Core::Rect & newRect,
                              GeometryChangeReason reason) override;
public:
    WIDGET_CONSTRUCTOR()
    explicit Container(const Core::Rect & rect,WidgetPtr parent);

    void setClampPolicy(const ContainerClampPolicy & policy);
    const ContainerClampPolicy & getClampPolicy() const;

    std::size_t childCount() const;
    Widget *childAt(std::size_t idx) const;
    WidgetPtr addChild(const WidgetPtr & child);
    bool removeChild(const WidgetPtr & child);

    void relayout();

    ~Container() override;

};

// /**
//  * @brief Similar to `Container` except all widgets can be moved (drag-dropped, animated) with native events or object methods.
//  * 
//  */

class OMEGAWTK_EXPORT AnimatedContainer : public Widget {
public:
    WIDGET_CONSTRUCTOR()
};


class OMEGAWTK_EXPORT ScrollableContainer : public Widget {
    WIDGET_CONSTRUCTOR()
};


}
#endif //OMEGAWTK_WIDGETS_BASICWIDGETS_H
