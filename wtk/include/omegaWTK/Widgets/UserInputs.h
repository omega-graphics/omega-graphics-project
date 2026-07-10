#include "BasicWidgets.h"
#include "WidgetTypes.h"
#include "omegaWTK/UI/UIView.h"
#include "omegaWTK/Composition/Brush.h"
#include "omegaWTK/Native/NativeTheme.h"
#include "omegaWTK/Native/NativeEvent.h"
#include "omegaWTK/Core/Core.h"
#include <functional>
#include <memory>

#ifndef OMEGAWTK_WIDGETS_USERINPUTS_H
#define OMEGAWTK_WIDGETS_USERINPUTS_H

namespace OmegaWTK {

    // -----------------------------------------------------------------------
    // TextInput — Phase 4B v0 base implementation.
    //
    // Single-line editable text field. Leaf Widget backed by one UIView with
    // three element tags:
    //   "bg"    — RoundedRect background (theme fill + focus-ring border)
    //   "label" — the typed text, or the dimmed placeholder when empty
    //   "caret" — 1px Rect insertion caret, authored ONLY while focused
    //
    // Focus: the UIView opts into `StrongFocus`, so a click (M1) or Tab
    // (F4) makes the FocusManager route KeyDown events here. An internal
    // `TextInputDelegate` (defined in UserInputs.TextInput.cpp) observes the
    // new `ViewDelegate::onFocusGained`/`onFocusLost` callbacks (to toggle
    // the caret + ring) and `onKeyDown` (to run the edit ops).
    //
    // v0 scope (Widget-Stub-Implementation-Plan §4B): printable insert,
    // Backspace, Delete, Left/Right arrow. NO selection, IME, or clipboard.
    // The caret X is a monospace approximation (per-glyph-exact caret is a
    // documented follow-up — see the .cpp header comment). Caret blink is a
    // follow-up too (needs a NativeTimer wire like the tooltip).
    // -----------------------------------------------------------------------

    class TextInputDelegate;

    struct OMEGAWTK_EXPORT TextInputProps {
        OmegaCommon::UString placeholder {};
        OmegaCommon::UString initialValue {};
        bool                 enabled = true;
        float                cornerRadius = 4.f;
    };

    class OMEGAWTK_EXPORT TextInput : public Widget {
        TextInputProps                                     props_;
        OmegaCommon::UString                               text_ {};
        std::size_t                                        caretPosition_ = 0;
        Native::ThemeDesc                                  theme_ {};
        bool                                               focused_ = false;
        std::function<void(const OmegaCommon::UString &)>  onValueChange_ {};
        Core::UniquePtr<TextInputDelegate>                 delegate_;

        friend class TextInputDelegate;

    protected:
        void onMount() override;
        void onThemeSet(Native::ThemeDesc & desc) override;
        void resize(Composition::Rect & newRect) override;

        // rebuildContent(): element list (bg / label / caret) + sub-rects.
        // rebuildStyle():   theme-driven colors + focus-ring on/off.
        void rebuildContent();
        void rebuildStyle();

        // Focus in/out — flips focused_, re-authors the caret, invalidates.
        void setFocused(bool focused);
        // Apply one key event (printable insert / edit / caret move). Fires
        // onValueChange_ when the text actually changes.
        void handleKey(const Native::KeyDownParams & params);

    public:
        explicit TextInput(Composition::Rect rect, const TextInputProps & props = {});
        ~TextInput() override;

        void setText(const OmegaCommon::UString & text);
        const OmegaCommon::UString & text() const { return text_; }

        void setOnValueChange(std::function<void(const OmegaCommon::UString &)> callback);
        void setEnabled(bool enabled);
        bool isEnabled() const;
        bool isFocused() const { return focused_; }
    };

    class OMEGAWTK_EXPORT Slider : public Widget {
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
