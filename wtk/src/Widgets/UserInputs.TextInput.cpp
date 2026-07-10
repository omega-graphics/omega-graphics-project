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
// Caret X — approximation, on purpose. The caret is placed with a fixed
// per-glyph advance (kApproxCharAdvance) rather than measured glyph widths.
// A measured caret would call `UIView::measureText` on the "label" tag, but
// that memo is keyed on (tag, availWidth) and only invalidated a frame later
// in `resolveStyles()`; measuring synchronously right after an edit returns
// the *previous* text's width (one keystroke stale), which looks worse than a
// clean approximation. Per-glyph-exact caret is a follow-up that needs
// `measureText` driven from the layout phase (or a content-revision cache
// key) — the width field added to `LayoutResult` (Text-Measurement-API-Plan
// §6) is the metric it will consume.
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
// Placeholder per-glyph advance for caret math. ~0.5em of the Arial-18 UA
// fallback the field renders in. See the file header for why this is not a
// measured advance in v0.
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

    const Composition::Rect r = rect();

    // bg — full-rect RoundedRect (fill + border come from Style).
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
        layout.element(spec);
    }

    const float contentLeft = r.pos.x + kHPad;
    const float contentW = std::max(0.f, r.w - kHPad * 2.f);

    // label — the typed text, or the placeholder when empty. Color +
    // alignment are applied in rebuildStyle().
    {
        const bool showPlaceholder = text_.empty();
        UIElementLayoutSpec spec;
        spec.tag = "label";
        spec.text = showPlaceholder ? props_.placeholder : text_;
        spec.textRect = Composition::Rect{
            Composition::Point2D{contentLeft, r.pos.y}, contentW, r.h};
        layout.element(spec);
    }

    // caret — a thin vertical Rect, authored only while focused + enabled.
    // Absent otherwise, so nothing paints when the field is not being edited.
    if(focused_ && props_.enabled) {
        const float caretX = std::min(
            contentLeft + static_cast<float>(caretPosition_) * kApproxCharAdvance,
            r.pos.x + r.w - kHPad);
        const float caretH = std::max(0.f, r.h - kCaretVPad * 2.f);
        Composition::Rect caretRect{
            Composition::Point2D{caretX, r.pos.y + kCaretVPad}, kCaretWidth, caretH};

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

    // caret color (cell is inert when the caret element is absent).
    ss->elementBrush("caret",
                     Composition::ColorBrush(cols.controlForeground),
                     false, 0.f);

    viewAs<UIView>().setStyle(ss);
}

// ---------------------------------------------------------------------------
// Focus + editing
// ---------------------------------------------------------------------------

void TextInput::setFocused(bool focused) {
    if(focused_ == focused) {
        return;
    }
    focused_ = focused;
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
