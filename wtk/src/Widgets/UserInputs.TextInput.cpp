#include "omegaWTK/Widgets/UserInputs.h"
#include "omegaWTK/UI/UIView.h"
#include "omegaWTK/UI/View.h"
#include "omegaWTK/Composition/Brush.h"
// Full CursorShape enum (View.h only forward-declares it, opaque, to avoid
// dragging NativeWindow.h's X11-macro handling into the public header — see
// Native-API-Completion-Proposal §2.3a C1). A .cpp may include it directly.
#include "omegaWTK/Native/NativeWindow.h"

#include <algorithm>
#include <memory>
#include <utility>

// ---------------------------------------------------------------------------
// TextInput — Phase 4B v0 (Widget-Stub-Implementation-Plan §4B).
//
// A single-line editable text field, unblocked by the FocusManager
// (Native-API-Completion-Proposal §2.3a). The UIView opts into StrongFocus,
// so a click (M1 click-focus) or Tab (F4 traversal) selects it and the
// FocusManager routes KeyDown events to its delegate.
//
// Caret X is measured: the advance of the prefix text_[0..caretPosition_) via
// `UIView::measureText("label", prefix, …)`, the substring overload that lays
// the prefix out fresh in the label's font (uncached, so it can't return a
// stale extent). Caret blink is a repeating NativeTimer that toggles the caret
// element's color while focused.
// ---------------------------------------------------------------------------

namespace OmegaWTK {

namespace {

constexpr float kHPad            = 8.f;   // text inset from the field's left edge
constexpr float kCaretVPad       = 5.f;   // caret inset from top/bottom
constexpr float kCaretWidth      = 1.5f;  // caret thickness (dp)
constexpr float kBorderWidth     = 1.f;   // resting hairline outline
constexpr float kFocusRingWidth  = 2.f;   // accent ring when focused
constexpr float kDisabledAlpha   = 0.4f;
constexpr float kPlaceholderAlpha = 0.5f;
constexpr float kCaretBlinkSec   = 0.5f;  // half-period of the caret blink
// Fallback per-glyph advance, used only if measurement is unavailable (no
// font/shaper resolved yet). Real caret placement measures the prefix.
constexpr float kApproxCharAdvance = 9.f;

} // anonymous namespace

// ---------------------------------------------------------------------------
// TextInputDelegate — focus + key routing.
// ---------------------------------------------------------------------------

class TextInputDelegate : public ViewDelegate {
    TextInput * owner_;
public:
    explicit TextInputDelegate(TextInput * owner) : ViewDelegate(), owner_(owner) {}

    void onFocusGained(Native::NativeEventPtr event) override {
        (void)event;
        owner_->setFocused(true);
    }
    void onFocusLost(Native::NativeEventPtr event) override {
        (void)event;
        owner_->setFocused(false);
    }
    void onKeyDown(Native::NativeEventPtr event) override {
        auto * params = static_cast<Native::KeyDownParams *>(event->params);
        if(params != nullptr) {
            owner_->handleKey(*params);
            // Consume so the key does not bubble to ancestor views
            // (Invariant A — a TextInput owns the keystrokes it edits with).
            event->handled = true;
        }
    }
    void onLeftMouseDown(Native::NativeEventPtr event) override {
        // The dispatcher's M1 click-focus already set focus on this view
        // before the delegate sees the event, so onFocusGained has run.
        // Click-to-place-caret is a v0 follow-up.
        (void)event;
    }
};

// ---------------------------------------------------------------------------
// Construction / lifetime
// ---------------------------------------------------------------------------

TextInput::TextInput(Composition::Rect rect, const TextInputProps & props)
    : Widget(ViewPtr(new UIView(rect, nullptr, "textinput"))),
      props_(props),
      delegate_(std::make_unique<TextInputDelegate>(this)) {
    // Seed the theme from the platform (onThemeSet only fires on *change*).
    theme_ = Native::queryCurrentTheme();
    text_ = props_.initialValue;
    caretPosition_ = text_.size();
    if(view != nullptr) {
        view->setDelegate(delegate_.get());
        // Opt into keyboard focus so the FocusManager routes keys here on
        // click / Tab. A disabled field is not focusable.
        view->setFocusPolicy(props_.enabled ? View::FocusPolicy::StrongFocus
                                            : View::FocusPolicy::NoFocus);
        // §2.3a C1: an editable text field shows the I-beam cursor.
        view->setCursorShape(Native::CursorShape::IBeam);
    }
}

TextInput::~TextInput() {
    // Stop the blink timer before members tear down so its `this`-capturing
    // callback can never fire against a half-destroyed field. (Dropping the
    // handle would also stop it, but being explicit documents the ordering.)
    stopCaretBlink();
    // delegate_ (UniquePtr) destroys after view; the view's raw delegate
    // pointer cannot dangle since both are Widget-owned members.
}

// ---------------------------------------------------------------------------
// Widget lifecycle hooks
// ---------------------------------------------------------------------------

void TextInput::onMount() {
    rebuildContent();
}

void TextInput::onThemeSet(Native::ThemeDesc & desc) {
    theme_ = desc;
    rebuildStyle();
}

void TextInput::resize(Composition::Rect & newRect) {
    viewAs<UIView>().resize(newRect);
    rebuildContent();
    invalidate(PaintReason::Resize);
}

// ---------------------------------------------------------------------------
// Element layout + style
// ---------------------------------------------------------------------------

void TextInput::rebuildContent() {
    // Build a fresh layout and publish it via setLayoutV2(), NOT by poking
    // the live layoutV2() in place. rebuildContent changes the element *set*
    // (the "caret" element toggles with focus, and its rect moves each edit),
    // and the in-place accessor only flips `layoutDirty` — it does not
    // markAllElementsDirty() or markDirty(Layout). Paired with
    // invalidate(StateChanged) (Paint-only), the arrange phase would never
    // re-run, so a set change paints from a stale arranged cache → the field
    // blanks until an unrelated full repaint. setLayoutV2() dirties
    // Style|Layout|Paint and re-keys the element node ids, which is exactly
    // what a per-edit content rebuild needs. (Button pokes layoutV2() in place
    // safely only because its element set never changes on a state change.)
    UIViewLayoutV2 layout;

    // Element geometry is authored in VIEW-LOCAL space (origin {0,0}); paint
    // clamps against the view's local bounds and then lifts everything into
    // window space by the view offset (UIView.Update.cpp paint: "authored /
    // clamped in view-local space"). Only the *size* of rect() is used here —
    // its absolute pos must NOT leak into element coords, or clampRectToParent
    // mangles small elements (the caret) differently from full-size ones (bg /
    // label clamp-to-fill and look fine, hiding the bug).
    const Composition::Rect r = rect();
    const float w = r.w;
    const float h = r.h;

    // bg — full-rect RoundedRect (fill + border come from Style).
    {
        Composition::RoundedRect bg{};
        bg.pos = Composition::Point2D{0.f, 0.f};
        bg.w = w;
        bg.h = h;
        bg.rad_x = props_.cornerRadius;
        bg.rad_y = props_.cornerRadius;

        UIElementLayoutSpec spec;
        spec.tag = "bg";
        spec.shape = Shape::RoundedRect(bg);
        layout.element(spec);
    }

    const float contentLeft = kHPad;
    const float contentW = std::max(0.f, w - kHPad * 2.f);

    // label — the typed text, or the placeholder when empty. Color +
    // alignment are applied in rebuildStyle().
    {
        const bool showPlaceholder = text_.empty();
        UIElementLayoutSpec spec;
        spec.tag = "label";
        spec.text = showPlaceholder ? props_.placeholder : text_;
        spec.textRect = Composition::Rect{
            Composition::Point2D{contentLeft, 0.f}, contentW, h};
        layout.element(spec);
    }

    // caret — a thin vertical Rect. Authored only on the visible half of the
    // blink cycle (gated on caretVisible_): the blink toggles the element's
    // *presence* via this rebuild rather than its color, because a Style-only
    // color flip did not reliably repaint the caret after the first toggle
    // (content-cache staleness on a scoped Style change); re-authoring routes
    // through setLayoutV2's full markAllElementsDirty + coherent re-submit.
    //   X = measured advance of the text before the caret (clamped to field).
    //   Height/Y = the glyph ink box at the engine's baseline (below).
    if(focused_ && props_.enabled && caretVisible_) {
        const float caretX = std::min(contentLeft + caretAdvanceDp(),
                                      w - kHPad);
        // Match the glyph ink exactly: the engine seats LeftCenter text with
        // baseline = ascent + (h - lineHeight)/2, ink = [ascent, descent] about
        // it, and the line gap trailing *below* the ink. A full-lineHeight box
        // would therefore hang below the text by the gap. So span the caret
        // over the ink box only, at the engine's baseline.
        const auto vm = viewAs<UIView>().resolvedTextMetrics("label");
        const float lineH = vm.ascent + vm.descent + vm.lineGap;
        float caretY;
        float caretH;
        if(lineH > 0.f) {
            const float extra = h - lineH;
            const float baseline = vm.ascent + (extra > 0.f ? extra * 0.5f : 0.f);
            caretY = baseline - vm.ascent;               // top of the ascent
            caretH = std::min(vm.ascent + vm.descent, h);
        } else {
            // No metrics yet — fall back to a centered inset box.
            caretH = std::max(0.f, h - kCaretVPad * 2.f);
            caretY = (h - caretH) * 0.5f;
        }
        Composition::Rect caretRect{
            Composition::Point2D{caretX, caretY}, kCaretWidth, caretH};

        UIElementLayoutSpec spec;
        spec.tag = "caret";
        spec.shape = Shape::Rect(caretRect);
        layout.element(spec);
    }

    viewAs<UIView>().setLayoutV2(layout);
    rebuildStyle();
}

void TextInput::rebuildStyle() {
    auto ss = Style::Create();
    const auto & cols = theme_.colors;
    const float fieldAlpha = props_.enabled ? 1.f : kDisabledAlpha;

    // bg fill.
    ss->elementBrush("bg",
                     Composition::ColorBrush(cols.controlBackground.withAlpha(fieldAlpha)),
                     /*transition=*/false, /*duration=*/0.f);

    // Border: accent focus ring while focused, hairline separator otherwise
    // (matches the Button focus-ring convention). elementBorder() is the
    // element-scoped border Paint actually consumes.
    if(focused_ && props_.enabled) {
        ss->elementBorder("bg", cols.accent, kFocusRingWidth, false, 0.f);
    } else {
        Composition::Color outline = cols.separator;
        if(!props_.enabled) {
            outline = outline.withAlpha(kDisabledAlpha);
        }
        ss->elementBorder("bg", outline, kBorderWidth, false, 0.f);
    }

    // label color — dimmed placeholder vs. full-strength text; both fade in
    // the disabled state.
    const bool showPlaceholder = text_.empty();
    Composition::Color labelColor = cols.controlForeground;
    if(showPlaceholder) {
        labelColor = labelColor.withAlpha(kPlaceholderAlpha * fieldAlpha);
    } else if(!props_.enabled) {
        labelColor = labelColor.withAlpha(kDisabledAlpha);
    }
    ss->textColor("label", labelColor, false, 0.f);
    // Left-aligned, vertically centered; never wrap (single line).
    ss->textAlignment("label", Composition::TextLayoutDescriptor::LeftCenter);
    ss->textWrapping("label", Composition::TextLayoutDescriptor::None);

    // caret color — always solid foreground. The blink is driven by the
    // caret element's presence in rebuildContent (gated on caretVisible_),
    // not by its color, so this cell is inert whenever the caret is absent.
    ss->elementBrush("caret",
                     Composition::ColorBrush(cols.controlForeground),
                     false, 0.f);

    viewAs<UIView>().setStyle(ss);
}

// ---------------------------------------------------------------------------
// Caret placement + blink
// ---------------------------------------------------------------------------

float TextInput::caretAdvanceDp() {
    if(caretPosition_ == 0 || text_.empty()) {
        return 0.f;
    }
    const OmegaCommon::UString prefix = text_.substr(0, caretPosition_);
    // Uncached substring overload: lays `prefix` out fresh in the label's font
    // so the advance tracks the exact glyphs, edit by edit. Large avail width
    // = no wrap (the field is single-line anyway).
    const float measured =
        viewAs<UIView>().measureText("label", prefix, 1.0e6f).width;
    if(measured > 0.f) {
        return measured;
    }
    // Font/shaper not resolvable yet (e.g. pre-first-frame): fall back to the
    // monospace estimate rather than collapsing the caret to the left edge.
    return static_cast<float>(caretPosition_) * kApproxCharAdvance;
}

void TextInput::startCaretBlink() {
    caretVisible_ = true;
    // Repeating timer on the main run loop. Capturing `this` is safe: the
    // timer is a member stopped in stopCaretBlink() / the destructor before
    // `this` goes away, and it only ever fires on the UI thread.
    blinkTimer_ = Native::make_native_timer(kCaretBlinkSec, /*repeats=*/true,
        [this](){
            caretVisible_ = !caretVisible_;
            rebuildContent();                     // toggles the caret element
            invalidate(PaintReason::StateChanged);
        });
}

void TextInput::stopCaretBlink() {
    if(blinkTimer_ != nullptr) {
        blinkTimer_->stop();
        blinkTimer_ = nullptr;
    }
    caretVisible_ = false;
}

void TextInput::resetCaretBlink() {
    // Editing snaps the caret solid and restarts the interval so it does not
    // blink off immediately after a keystroke (matches native fields).
    caretVisible_ = true;
    if(blinkTimer_ != nullptr) {
        blinkTimer_->start();   // restart from now
    }
}

// ---------------------------------------------------------------------------
// Focus + editing
// ---------------------------------------------------------------------------

void TextInput::setFocused(bool focused) {
    if(focused_ == focused) {
        return;
    }
    focused_ = focused;
    // Start/stop the blink around the state flip so caretVisible_ is settled
    // before rebuildContent authors (and rebuildStyle colors) the caret.
    if(focused_) {
        startCaretBlink();
    } else {
        stopCaretBlink();
    }
    // Re-author so the caret element appears / disappears and the border
    // switches between the focus ring and the resting hairline.
    rebuildContent();
    invalidate(PaintReason::StateChanged);
}

void TextInput::handleKey(const Native::KeyDownParams & params) {
    if(!props_.enabled) {
        return;
    }

    bool textChanged = false;
    bool caretMoved  = false;

    switch(params.code) {
        case Native::KeyCode::Backspace:
            if(caretPosition_ > 0) {
                text_.erase(caretPosition_ - 1, 1);
                --caretPosition_;
                textChanged = true;
            }
            break;
        case Native::KeyCode::Delete:
            if(caretPosition_ < text_.size()) {
                text_.erase(caretPosition_, 1);
                textChanged = true;
            }
            break;
        case Native::KeyCode::ArrowLeft:
            if(caretPosition_ > 0) {
                --caretPosition_;
                caretMoved = true;
            }
            break;
        case Native::KeyCode::ArrowRight:
            if(caretPosition_ < text_.size()) {
                ++caretPosition_;
                caretMoved = true;
            }
            break;
        case Native::KeyCode::Home:
            if(caretPosition_ != 0) { caretPosition_ = 0; caretMoved = true; }
            break;
        case Native::KeyCode::End:
            if(caretPosition_ != text_.size()) { caretPosition_ = text_.size(); caretMoved = true; }
            break;
        default: {
            // Printable insert. Skip when a Control/Meta chord is held (those
            // are shortcuts, not text — Cmd+A etc.), and skip control code
            // points (Enter 0x0D, Tab, DEL 0x7F). Shift/Alt are already baked
            // into `key` by the backend.
            const OmegaCommon::Unicode32Char ch = params.key;
            const bool chord = params.modifiers.control || params.modifiers.meta;
            if(!chord && ch >= 0x20 && ch != 0x7F) {
                text_.insert(caretPosition_, 1, ch);
                ++caretPosition_;
                textChanged = true;
            }
            break;
        }
    }

    if(textChanged || caretMoved) {
        resetCaretBlink();   // snap caret solid + restart interval on any edit
        rebuildContent();
        invalidate(PaintReason::StateChanged);
    }
    if(textChanged && onValueChange_) {
        onValueChange_(text_);
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void TextInput::setText(const OmegaCommon::UString & text) {
    text_ = text;
    caretPosition_ = text_.size();
    rebuildContent();
    invalidate(PaintReason::StateChanged);
}

void TextInput::setOnValueChange(std::function<void(const OmegaCommon::UString &)> callback) {
    onValueChange_ = std::move(callback);
}

void TextInput::setEnabled(bool enabled) {
    if(props_.enabled == enabled) {
        return;
    }
    props_.enabled = enabled;
    if(view != nullptr) {
        view->setFocusPolicy(enabled ? View::FocusPolicy::StrongFocus
                                     : View::FocusPolicy::NoFocus);
    }
    if(!enabled && focused_) {
        // Dropping focusability while focused: relinquish focus so the
        // caret clears and keys stop routing here.
        focused_ = false;
        if(view != nullptr) {
            view->blur();
        }
    }
    rebuildContent();
    invalidate(PaintReason::StateChanged);
}

bool TextInput::isEnabled() const {
    return props_.enabled;
}

}
