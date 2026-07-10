#include "omegaWTK/Widgets/WidgetTypes.h"
#include "omegaWTK/UI/Widget.h"

namespace OmegaWTK {

WidgetInteractionDelegate::WidgetInteractionDelegate(Widget *owner)
    : ViewDelegate(), owner(owner) {}

InteractiveState WidgetInteractionDelegate::getState() const {
    return state;
}

bool WidgetInteractionDelegate::isDisabled() const {
    return state == InteractiveState::Disabled;
}

void WidgetInteractionDelegate::setDisabled(bool disabled) {
    if (disabled) {
        state = InteractiveState::Disabled;
    } else if (state == InteractiveState::Disabled) {
        state = InteractiveState::Idle;
    }
    if (owner) owner->invalidate();
}

// --- Mouse state machine ---

void WidgetInteractionDelegate::onMouseEnter(Native::NativeEventPtr event) {
    (void)event;
    if (state == InteractiveState::Disabled) return;
    state = InteractiveState::Hovered;
    if (owner) owner->invalidate();
}

void WidgetInteractionDelegate::onMouseExit(Native::NativeEventPtr event) {
    (void)event;
    if (state == InteractiveState::Disabled) return;
    state = InteractiveState::Idle;
    if (owner) owner->invalidate();
}

void WidgetInteractionDelegate::onLeftMouseDown(Native::NativeEventPtr event) {
    // ScrollView-4.7-Integration-Plan V2.2, Invariant A: an interactive
    // widget consumes the left-click it receives so it does not bubble to
    // an ancestor (e.g. a Button inside a ScrollView must not let the click
    // fall through). A disabled widget still absorbs the click — it does
    // not fall through to something behind it — it just does not react.
    if (event) event->handled = true;
    if (state == InteractiveState::Disabled) return;
    state = InteractiveState::Pressed;
    if (owner) owner->invalidate();
}

void WidgetInteractionDelegate::onLeftMouseUp(Native::NativeEventPtr event) {
    if (event) event->handled = true;
    if (state == InteractiveState::Disabled) return;
    state = InteractiveState::Hovered;
    if (owner) owner->invalidate();
}

// --- Key events (placeholder for focus management) ---

void WidgetInteractionDelegate::onKeyDown(Native::NativeEventPtr event) {
    (void)event;
}

void WidgetInteractionDelegate::onKeyUp(Native::NativeEventPtr event) {
    (void)event;
}

}
