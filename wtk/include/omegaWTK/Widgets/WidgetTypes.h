
#include "omegaWTK/UI/View.h"

#ifndef OMEGAWTK_WIDGETS_WIDGETTYPES_H
#define OMEGAWTK_WIDGETS_WIDGETTYPES_H

namespace OmegaWTK {

class Widget;

enum class InteractiveState : uint8_t {
    Idle,
    Hovered,
    Pressed,
    Focused,
    Disabled
};

enum class Orientation : uint8_t {
    Horizontal,
    Vertical
};

/// Reusable ViewDelegate subclass that tracks hover/press/focus state
/// and invalidates the owning widget on each transition.
/// Interactive widgets embed one of these instead of duplicating
/// pointer-tracking logic.
class OMEGAWTK_EXPORT WidgetInteractionDelegate : public ViewDelegate {
protected:
    InteractiveState state = InteractiveState::Idle;
    Widget *owner = nullptr;

    void onMouseEnter(Native::NativeEventPtr event) override;
    void onMouseExit(Native::NativeEventPtr event) override;
    void onLeftMouseDown(Native::NativeEventPtr event) override;
    void onLeftMouseUp(Native::NativeEventPtr event) override;
    void onKeyDown(Native::NativeEventPtr event) override;
    void onKeyUp(Native::NativeEventPtr event) override;
public:
    explicit WidgetInteractionDelegate(Widget *owner);

    InteractiveState getState() const;
    bool isDisabled() const;
    void setDisabled(bool disabled);
};

}

#endif // OMEGAWTK_WIDGETS_WIDGETTYPES_H
