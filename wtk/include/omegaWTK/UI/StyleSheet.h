#ifndef OMEGAWTK_UI_STYLESHEET_H
#define OMEGAWTK_UI_STYLESHEET_H

#include "omegaWTK/Core/Core.h"
#include "omegaWTK/UI/StyleProperty.h"           // PropertyKey, StyleValue, AnimatedValue
#include "omegaWTK/Composition/Animation.h"      // KeyframeTrack, TimingOptions, AnimationCurve

#include <cstdint>

namespace OmegaWTK {

/// Widget-View-Paint-Lifecycle-Plan Tier D / D6.1 (2026-06-03):
/// the new style-sheet vocabulary. Selectors, rules, sheets, and
/// keyframe-animation declarations. Lives in its own namespace so
/// the names don't collide with the legacy `OmegaWTK::StyleRule`
/// declared in `omegaWTK/UI/Layout.h` (a layout-only struct from
/// before the resolved-style table existed).
namespace StyleSheets {

// ---------------------------------------------------------------
// Pseudo-class state
// ---------------------------------------------------------------

/// D6.4 wires these onto `View` as a state bitmask; D6.1 just
/// defines the value space so selectors can name them.
enum class PseudoClass : std::uint8_t {
    None     = 0,
    Hover    = 1U << 0,
    Pressed  = 1U << 1,
    Focused  = 1U << 2,
    Disabled = 1U << 3,
};

inline PseudoClass operator|(PseudoClass a, PseudoClass b){
    return static_cast<PseudoClass>(
        static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}
inline PseudoClass operator&(PseudoClass a, PseudoClass b){
    return static_cast<PseudoClass>(
        static_cast<std::uint8_t>(a) & static_cast<std::uint8_t>(b));
}
inline PseudoClass & operator|=(PseudoClass & a, PseudoClass b){
    a = a | b;
    return a;
}
inline PseudoClass & operator&=(PseudoClass & a, PseudoClass b){
    a = a & b;
    return a;
}
/// True iff every bit in `mask` is set in `bits`.
inline bool contains(PseudoClass bits, PseudoClass mask){
    return (static_cast<std::uint8_t>(bits) &
            static_cast<std::uint8_t>(mask)) ==
           static_cast<std::uint8_t>(mask);
}

// ---------------------------------------------------------------
// Selector
// ---------------------------------------------------------------

/// Tier-1 single-compound selector: one optional tag + one optional
/// id + zero-or-more class tokens + a pseudo-class subset. A node
/// matches when every present constraint matches. Empty `tag` /
/// empty `id` mean "no constraint on that axis"; an empty
/// `classes` vector means "no class constraint." `pseudoClasses`
/// requires every set bit to be set on the node (subset match), so
/// `Hover|Pressed` requires BOTH bits — the conventional CSS combinator
/// semantics.
///
/// Tier 2+ (selector lists, descendants, combinators, `:not(...)`)
/// lives in later tiers; D6 only ships single-compound.
struct OMEGAWTK_EXPORT Selector {
    OmegaCommon::String                       tag {};
    OmegaCommon::String                       id  {};
    OmegaCommon::Vector<OmegaCommon::String>  classes {};
    PseudoClass                               pseudoClasses = PseudoClass::None;

    /// CSS specificity convention: `id * 100 + (class + pseudo) * 10 + tag`.
    /// A pseudo-class bit weighs the same as one class. Used by
    /// `StyleRule::beats` for cascade ordering.
    int specificity() const;
};

// ---------------------------------------------------------------
// Transition + keyframe binding metadata
// ---------------------------------------------------------------

/// `transition:` metadata recorded on a rule. D6.5 records the
/// binding; D7.2 wires the actual `scheduler.transition(...)` call
/// when a resolved property changes between frames.
struct OMEGAWTK_EXPORT TransitionSpec {
    PropertyKey                                  key;
    Composition::TimingOptions                   timing {};
    SharedHandle<Composition::AnimationCurve>    curve = nullptr;
};

/// One per-property track of a keyframe animation. The track is
/// type-erased over the animation-side `AnimatedValue` variant; D7.3
/// wires the firing-time lerp specialization.
struct OMEGAWTK_EXPORT KeyframeAnimationProperty {
    PropertyKey                                  key = PropertyKey::Opacity;
    Composition::KeyframeTrack<AnimatedValue>    track {};
};

/// A named keyframe animation declaration on a sheet. D7.3 fires it
/// when a rule with `animation: <name>` matches a node.
struct OMEGAWTK_EXPORT KeyframeAnimation {
    OmegaCommon::String                              name {};
    OmegaCommon::Vector<KeyframeAnimationProperty>   properties {};
    Composition::TimingOptions                       defaultTiming {};
};

// ---------------------------------------------------------------
// StyleRule
// ---------------------------------------------------------------

/// One declaration block: a selector + a property→value map +
/// optional transition / keyframe-animation metadata. The
/// `declarations` map is keyed by `PropertyKey` and valued as a
/// `StyleValue` — the same per-property cell shape the D5 resolved-
/// style table holds, so the StyleResolver can copy through without
/// per-property conversion code.
struct OMEGAWTK_EXPORT StyleRule {
    Selector              selector {};
    std::size_t           sourceOrder = 0;
    OmegaCommon::Map<PropertyKey, StyleValue>           declarations {};
    OmegaCommon::Vector<TransitionSpec>                 transitions {};
    /// `animation: <name>` binding — references a `KeyframeAnimation`
    /// declared on the same sheet. D6.5 records; D7.3 fires.
    Core::Optional<OmegaCommon::String>                 animationName {};

    /// Cascade comparator: higher specificity wins; tie-break by
    /// later `sourceOrder` (the conventional last-rule-wins). Mirrors
    /// the `OmegaWTK::StyleRule::beats()` shape in `Layout.h:250`,
    /// adapted for the new compound `Selector`.
    bool beats(const StyleRule & other) const;

    // ---- typed declaration setters (D6.1 convenience) -----------
    // Each setter writes one cell into `declarations` and returns
    // `*this` so app / test code can chain a rule together without
    // hand-wrapping each value in a `StyleValue{...}`. The setter
    // family is non-exhaustive — it covers the cell shapes Paint
    // currently reads (D5 mapping) plus the obvious extensions.
    // Sheet authors who need a property without a setter can
    // assign `declarations[Key] = StyleValue{...}` directly.
    StyleRule & setBackgroundColor(Composition::Color color);
    StyleRule & setBorderColor(Composition::Color color);
    StyleRule & setBorderWidth(std::uint32_t widthPx);
    StyleRule & setFillBrush(SharedHandle<Composition::Brush> brush);
    StyleRule & setDropShadow(Composition::LayerEffect::DropShadowParams params);
    StyleRule & setTextFont(SharedHandle<Composition::Font> font);
    StyleRule & setTextColor(Composition::Color color);
    StyleRule & setTextLayout(Composition::TextLayoutDescriptor layout);
    StyleRule & setTextLineLimit(std::uint32_t lineLimit);
};

// ---------------------------------------------------------------
// StyleSheet
// ---------------------------------------------------------------

class StyleSheet;
OMEGACOMMON_SHARED_CLASS(StyleSheet);

/// A read-only bundle of `StyleRule`s + named `KeyframeAnimation`s.
/// Sheets are immutable-once-installed (per §0.3 #1) — to add a rule
/// or mutate metadata, copy the sheet through a `Builder`, which
/// produces a new `SharedHandle<StyleSheet>`. The handle is shareable
/// across `AppWindow`s.
class OMEGAWTK_EXPORT StyleSheet {
public:
    static SharedHandle<StyleSheet> Create();

    /// Builder produces an immutable handle. The builder may be
    /// reused after `build()` to seed a sibling sheet.
    class OMEGAWTK_EXPORT Builder {
    public:
        Builder & addRule(StyleRule rule);
        Builder & addKeyframeAnimation(KeyframeAnimation animation);
        SharedHandle<StyleSheet> build() const;
    private:
        OmegaCommon::Vector<StyleRule>                                rules_ {};
        OmegaCommon::Map<OmegaCommon::String, KeyframeAnimation>      keyframes_ {};
        std::size_t                                                   nextSourceOrder_ = 0;
    };

    const OmegaCommon::Vector<StyleRule> & rules() const;
    const OmegaCommon::Map<OmegaCommon::String, KeyframeAnimation> &
        keyframeAnimations() const;

private:
    StyleSheet() = default;
    friend class Builder;

    OmegaCommon::Vector<StyleRule>                                rules_ {};
    OmegaCommon::Map<OmegaCommon::String, KeyframeAnimation>      keyframes_ {};
};

} // namespace StyleSheets

// Widget-View-Paint-Lifecycle-Plan Tier D / D6.1 (2026-06-03):
// The `OmegaWTK::StyleSheets` namespace holds the new D6 sheet
// vocabulary. It would have been called `Style` to match the §5 D6
// sketch verbatim, but the legacy inline-style aggregate at
// `omegaWTK/UI/UIView.h:19` (`struct OmegaWTK::Style`) already owns
// that identifier. When the inline-style aggregate retires (a later
// tier), this namespace can be aliased to `Style` if desired.

} // namespace OmegaWTK

#endif // OMEGAWTK_UI_STYLESHEET_H
