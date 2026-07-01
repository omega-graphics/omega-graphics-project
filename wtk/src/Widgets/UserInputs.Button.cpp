#include "omegaWTK/Widgets/UserInputs.h"
#include "omegaWTK/Widgets/WidgetTypes.h"
#include "omegaWTK/UI/UIView.h"
#include "omegaWTK/UI/View.h"
#include "omegaWTK/Composition/Brush.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace OmegaWTK {

// ---------------------------------------------------------------------------
// Local helpers
// ---------------------------------------------------------------------------

namespace {

constexpr float kHPad   = 12.f;
constexpr float kIconGap = 6.f;
constexpr float kFocusRingWidth = 2.f;
constexpr float kBorderWidth = 1.f;   // resting outline (see rebuildStyle)
constexpr float kDisabledAlpha  = 0.4f;
constexpr float kHoverAccentMix = 0.10f;   // % of accent blended into bg on hover

/// Luminance-based contrast pick. Returns white on dark backgrounds, black
/// on light backgrounds. Same heuristic Chromium's views::Button uses for
/// disabled-on-accent labels. Good enough until a designer overrides via
/// `labelColorOverride`.
Composition::Color contrastOn(const Composition::Color & bg){
    const float luminance = bg.r * 0.299f + bg.g * 0.587f + bg.b * 0.114f;
    return luminance > 0.5f ? Composition::Color::Black
                            : Composition::Color::White;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// ButtonInteractionDelegate
// ---------------------------------------------------------------------------

class ButtonInteractionDelegate : public WidgetInteractionDelegate {
    Button * button_;
    /// True between an internal mouseDown and the matching mouseUp. Cleared
    /// on mouseExit so a drag-off cancels the click. The base class's
    /// `state` field already tells us *visually* what state we're in;
    /// `pendingClick_` is the extra bit needed to distinguish
    /// "mouseUp inside" (fire) from "mouseUp after drag-off" (no fire).
    bool pendingClick_ = false;

public:
    explicit ButtonInteractionDelegate(Button * b)
        : WidgetInteractionDelegate(b), button_(b) {}

    void onMouseEnter(Native::NativeEventPtr event) override {
        const auto prev = state;
        WidgetInteractionDelegate::onMouseEnter(event);
        if(prev != state) {
            button_->onInteractionStateChanged(state, /*clickConfirmed=*/false);
        }
    }

    void onMouseExit(Native::NativeEventPtr event) override {
        pendingClick_ = false;
        const auto prev = state;
        WidgetInteractionDelegate::onMouseExit(event);
        if(prev != state) {
            button_->onInteractionStateChanged(state, /*clickConfirmed=*/false);
        }
    }

    void onLeftMouseDown(Native::NativeEventPtr event) override {
        const auto prev = state;
        WidgetInteractionDelegate::onLeftMouseDown(event);
        if(state == InteractiveState::Pressed) {
            pendingClick_ = true;
        }
        if(prev != state) {
            button_->onInteractionStateChanged(state, /*clickConfirmed=*/false);
        }
    }

    void onLeftMouseUp(Native::NativeEventPtr event) override {
        const bool fired = pendingClick_;
        pendingClick_ = false;
        const auto prev = state;
        WidgetInteractionDelegate::onLeftMouseUp(event); // Pressed -> Hovered
        if(prev != state || fired) {
            button_->onInteractionStateChanged(state, /*clickConfirmed=*/fired);
        }
    }
};

// ---------------------------------------------------------------------------
// Button — construction / lifetime
// ---------------------------------------------------------------------------

Button::Button(Composition::Rect rect, const ButtonProps & props)
    : Widget(ViewPtr(new UIView(rect, nullptr, "button"))),
      props_(props),
      delegate_(std::make_unique<ButtonInteractionDelegate>(this)) {
    // Seed the theme from the platform query so the first paint reflects
    // the current Light/Dark appearance — the framework's onThemeSet only
    // fires on theme *change*, not on construction. The cached theme_
    // gets refreshed by onThemeSet whenever the user switches appearance.
    theme_ = Native::queryCurrentTheme();
    if(view != nullptr) {
        view->setDelegate(delegate_.get());
    }
    if(!props_.enabled) {
        delegate_->setDisabled(true);
        state_ = InteractiveState::Disabled;
    }
}

Button::~Button() {
    // delegate_ destroys via UniquePtr after view; nothing manual needed.
    // The view's raw setDelegate pointer becomes dangling only if the
    // view outlives the Button, which it cannot — view is a Widget member.
}

// ---------------------------------------------------------------------------
// Widget lifecycle hooks
// ---------------------------------------------------------------------------

void Button::onMount() {
    rebuildContent();
}

void Button::onThemeSet(Native::ThemeDesc & desc) {
    theme_ = desc;
    rebuildStyle(/*animate=*/false);
}

void Button::resize(Composition::Rect & newRect) {
    viewAs<UIView>().resize(newRect);
    rebuildContent();
    invalidate(PaintReason::Resize);
}

// ---------------------------------------------------------------------------
// Element layout + style — the two intentional rebuild paths.
// ---------------------------------------------------------------------------

void Button::rebuildContent() {
    auto & uv = viewAs<UIView>();
    auto & lv2 = uv.layoutV2();
    lv2.clear();

    const Composition::Rect r = rect();

    // bg — full-rect RoundedRect.
    {
        Composition::RoundedRect bg{};
        bg.pos = r.pos;
        bg.w = r.w;
        bg.h = r.h;
        bg.rad_x = props_.cornerRadius;
        bg.rad_y = props_.cornerRadius;

        UIElementLayoutSpec spec;
        spec.tag = "bg";
        spec.shape = Shape::RoundedRect(bg);
        lv2.element(spec);
    }

    // Compute content sub-rects: icon (left, square, vertically centered)
    // followed by label (fills remaining horizontal space).
    float cursorX = r.pos.x + kHPad;
    const float availW = std::max(0.f, r.w - kHPad * 2.f);

    if(!props_.iconToken.empty()) {
        const float iconSide = std::min(r.h - 4.f, 16.f);
        UIElementLayoutSpec iconSpec;
        iconSpec.tag = "icon";
        // The text element renders the token glyph; per Phase 2B (deferred),
        // a future IconRegistry can map tokens to bitmap/SVG instead.
        iconSpec.text = OmegaCommon::UString(props_.iconToken.begin(),
                                             props_.iconToken.end());
        iconSpec.textRect = Composition::Rect{
            Composition::Point2D{cursorX, r.pos.y + (r.h - iconSide) * 0.5f},
            iconSide, iconSide
        };
        lv2.element(iconSpec);
        cursorX += iconSide + kIconGap;
    }

    // label — fills whatever's left of the content rect.
    {
        const float labelX = cursorX;
        const float labelW = std::max(0.f, (r.pos.x + r.w - kHPad) - labelX);
        UIElementLayoutSpec labelSpec;
        labelSpec.tag = "label";
        labelSpec.text = props_.text;
        labelSpec.textRect = Composition::Rect{
            Composition::Point2D{labelX, r.pos.y},
            labelW, r.h
        };
        (void)availW; // referenced for future flex math; quiet -Wunused
        lv2.element(labelSpec);
    }

    rebuildStyle(/*animate=*/false);
}

void Button::rebuildStyle(bool animate) {
    auto ss = Style::Create();

    // Resolve the current state's bg + label colors from the theme +
    // overrides. The state -> color mapping is the table in
    // Widget-Stub-Implementation-Plan §4A.
    const auto & cols = theme_.colors;
    const auto accent = props_.tintOverride.value_or(cols.accent);

    Composition::Color bgColor;
    Composition::Color labelColor;
    bool drawFocusRing = false;

    switch(state_) {
        case InteractiveState::Idle:
            bgColor = cols.controlBackground;
            labelColor = props_.labelColorOverride.value_or(cols.controlForeground);
            break;
        case InteractiveState::Hovered:
            bgColor = Composition::Color::lerp(cols.controlBackground, accent, kHoverAccentMix);
            labelColor = props_.labelColorOverride.value_or(cols.controlForeground);
            break;
        case InteractiveState::Pressed:
            bgColor = accent;
            labelColor = props_.labelColorOverride.value_or(contrastOn(accent));
            break;
        case InteractiveState::Focused:
            // Focus is unreachable today (FocusManager not yet shipped —
            // see Native-API-Completion-Proposal §2.3a). The branch stays
            // so the ring renders correctly when FocusManager lands.
            bgColor = cols.controlBackground;
            labelColor = props_.labelColorOverride.value_or(cols.controlForeground);
            drawFocusRing = true;
            break;
        case InteractiveState::Disabled:
            bgColor = cols.controlBackground.withAlpha(kDisabledAlpha);
            labelColor = props_.labelColorOverride.value_or(cols.controlForeground)
                            .withAlpha(kDisabledAlpha);
            break;
    }

    // Per-state transition duration. Idle/Hovered/Disabled animate softly;
    // Pressed snaps for tactile feedback.
    const bool snap = (state_ == InteractiveState::Pressed)
                      || !animate
                      || props_.hoverTransitionDuration <= 0.f;
    const float duration = snap
        ? props_.pressTransitionDuration
        : props_.hoverTransitionDuration;
    const bool useTransition = !snap;

    ss->elementBrush("bg",
                     Composition::ColorBrush(bgColor),
                     /*transition=*/useTransition,
                     /*duration=*/duration);

    // Resting outline on the "bg" element. Always draw a subtle 1px
    // border so the button stays visible when its fill matches the window
    // surface — notably light mode, where controlBackground ≈
    // windowBackground and a fill-only button vanishes into the page. The
    // Focused state promotes it to the thicker accent focus ring.
    // elementBorder() (element-scoped) is the border path Paint actually
    // consumes; the old view-scoped border()/borderColor()/borderWidth()
    // resolve into ResolvedViewStyle but are never painted.
    if(drawFocusRing) {
        ss->elementBorder("bg", accent, kFocusRingWidth, useTransition, duration);
    } else {
        // The OS separator color is the semantic "hairline border" hue
        // and tracks the theme (light-gray in Light, dark-gray in Dark),
        // so the outline reads correctly in both appearances. Dim it in
        // the disabled state to match the faded fill + label.
        Composition::Color outline = cols.separator;
        if(state_ == InteractiveState::Disabled) {
            outline = outline.withAlpha(kDisabledAlpha);
        }
        ss->elementBorder("bg", outline, kBorderWidth, useTransition, duration);
    }

    if(!props_.iconToken.empty()) {
        ss->textColor("icon",
                      labelColor,
                      /*transition=*/useTransition,
                      /*duration=*/duration);
    }
    ss->textColor("label",
                  labelColor,
                  /*transition=*/useTransition,
                  /*duration=*/duration);
    ss->textAlignment("label", Composition::TextLayoutDescriptor::MiddleCenter);
    ss->textWrapping("label", Composition::TextLayoutDescriptor::None);

    if(!props_.iconToken.empty()) {
        ss->textAlignment("icon", Composition::TextLayoutDescriptor::MiddleCenter);
        ss->textWrapping("icon", Composition::TextLayoutDescriptor::None);
    }

    viewAs<UIView>().setStyle(ss);
}

// ---------------------------------------------------------------------------
// Interaction state transition handler
// ---------------------------------------------------------------------------

void Button::onInteractionStateChanged(InteractiveState newState,
                                       bool clickConfirmed) {
    const bool stateActuallyChanged = (state_ != newState);
    state_ = newState;
    if(stateActuallyChanged) {
        rebuildStyle(/*animate=*/true);
        invalidate(PaintReason::StateChanged);
    }
    if(clickConfirmed && onPress_) {
        onPress_();
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void Button::setProps(const ButtonProps & props) {
    const bool enabledChanged = (props.enabled != props_.enabled);
    const bool layoutChanged  = (props.text       != props_.text       ||
                                 props.iconToken  != props_.iconToken  ||
                                 props.cornerRadius != props_.cornerRadius);
    props_ = props;

    // Propagate enabled into the delegate + interactive state cache so
    // the next mouse event sees the correct disabled gate.
    if(enabledChanged) {
        delegate_->setDisabled(!props_.enabled);
        state_ = props_.enabled ? InteractiveState::Idle : InteractiveState::Disabled;
    }

    if(layoutChanged) {
        rebuildContent();
    } else {
        rebuildStyle(/*animate=*/false);
    }
    invalidate(PaintReason::StateChanged);
}

void Button::setOnPress(std::function<void()> callback) {
    onPress_ = std::move(callback);
}

void Button::setEnabled(bool enabled) {
    if(props_.enabled == enabled) {
        return;
    }
    props_.enabled = enabled;
    delegate_->setDisabled(!enabled);
    state_ = enabled ? InteractiveState::Idle : InteractiveState::Disabled;
    rebuildStyle(/*animate=*/true);
    invalidate(PaintReason::StateChanged);
}

bool Button::isEnabled() const {
    return props_.enabled;
}

}
