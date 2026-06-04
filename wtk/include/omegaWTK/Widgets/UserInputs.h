#include "BasicWidgets.h"
#include "WidgetTypes.h"
#include "omegaWTK/UI/UIView.h"
#include "omegaWTK/Composition/Brush.h"
#include "omegaWTK/Native/NativeTheme.h"
#include "omegaWTK/Core/Core.h"
#include <functional>
#include <memory>

#ifndef OMEGAWTK_WIDGETS_USERINPUTS_H
#define OMEGAWTK_WIDGETS_USERINPUTS_H

namespace OmegaWTK {

    // TextInput, Dropdown, Slider remain stubs (their base implementations
    // are gated on FocusManager — see Widget-Stub-Implementation-Plan §4B
    // / Native-API-Completion-Proposal §2.3a).
    class OMEGAWTK_EXPORT TextInput : public Container {
    public:
        WIDGET_CONSTRUCTOR()
    };

    class OMEGAWTK_EXPORT Slider : public Container {
    public:
        WIDGET_CONSTRUCTOR()
    };

    // -----------------------------------------------------------------------
    // Button — Phase 4A base implementation.
    //
    // Leaf Widget backed by a single UIView with three element tags:
    //   "bg"    — RoundedRect background (state-driven fill / focus ring)
    //   "icon"  — optional glyph (hidden when iconToken is empty)
    //   "label" — text
    //
    // Internal `ButtonInteractionDelegate` (defined in
    // UserInputs.Button.cpp) tracks hover/press state via the
    // `WidgetInteractionDelegate` base and fires `onPress_` when a
    // mouseUp lands inside the widget after a mouseDown — drag-off
    // cancels the click.
    //
    // Theme integration: `onThemeSet` caches the `ThemeDesc::Colors`;
    // `rebuildStyle()` derives the per-state Style from the cache. State
    // transitions only re-apply the Style (not the element layout) and
    // animate the bg fill / label color with a short transition.
    //
    // The full spec lives in
    // wtk/.plans/Widget-Stub-Implementation-Plan.md §4A.
    // -----------------------------------------------------------------------

    class ButtonInteractionDelegate;

    struct OMEGAWTK_EXPORT ButtonProps {
        OmegaCommon::UString text {};
        OmegaCommon::String  iconToken {};            // empty => no icon
        bool                 enabled      = true;
        float                cornerRadius = 4.f;
        // Optional overrides — when unset, theme colors are used.
        Core::Optional<Composition::Color> tintOverride {};       // bg accent in hover/pressed
        Core::Optional<Composition::Color> labelColorOverride {};
        // Transition duration (seconds) applied to bg / label color changes
        // on state transitions. 0 disables animation entirely.
        float hoverTransitionDuration = 0.15f;
        float pressTransitionDuration = 0.f;   // instant on press for snappy feel
    };

    class OMEGAWTK_EXPORT Button : public Widget {
        ButtonProps                                       props_;
        Native::ThemeDesc                                 theme_ {};
        InteractiveState                                  state_ = InteractiveState::Idle;
        std::function<void()>                             onPress_ {};
        Core::UniquePtr<ButtonInteractionDelegate>        delegate_;

        friend class ButtonInteractionDelegate;

    protected:
        void onMount() override;
        void onThemeSet(Native::ThemeDesc & desc) override;
        void resize(Composition::Rect & newRect) override;

        // Two rebuild paths, split intentionally:
        //  - rebuildContent(): element list + sub-rects. Layout-changing.
        //    Runs on onMount / resize / setProps.
        //  - rebuildStyle(animate): per-state Style only. Runs on every
        //    interaction state change — much cheaper than re-laying out.
        //    `animate=true` applies the props-configured transition
        //    duration; `animate=false` snaps the style.
        void rebuildContent();
        void rebuildStyle(bool animate);

        void onInteractionStateChanged(InteractiveState newState,
                                       bool clickConfirmed);

    public:
        explicit Button(Composition::Rect rect, const ButtonProps & props = {});
        ~Button() override;

        void setProps(const ButtonProps & props);
        const ButtonProps & props() const { return props_; }

        void setOnPress(std::function<void()> callback);
        void setEnabled(bool enabled);
        bool isEnabled() const;

        InteractiveState interactionState() const { return state_; }
    };

}

#endif //OMEGAWTK_WIDGETS_USERINPUTS_H
