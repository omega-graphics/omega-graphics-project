#include "omegaWTK/UI/StyleSheet.h"

#include <utility>

namespace OmegaWTK::StyleSheets {

// ---------------------------------------------------------------
// Selector
// ---------------------------------------------------------------

int Selector::specificity() const {
    // CSS convention: id * 100 + (class + pseudo + customState) * 10
    // + tag. Each pseudo-class bit and each custom-state name weighs
    // as one class (Widget-View-Paint-Lifecycle-Plan D7.4,
    // 2026-06-04).
    int spec = 0;
    if(!id.empty()){
        spec += 100;
    }
    spec += static_cast<int>(classes.size()) * 10;
    // count set bits in pseudoClasses
    auto pcBits = static_cast<std::uint8_t>(pseudoClasses);
    int pcCount = 0;
    while(pcBits != 0){
        pcCount += static_cast<int>(pcBits & 1U);
        pcBits >>= 1U;
    }
    spec += pcCount * 10;
    spec += static_cast<int>(customStates.size()) * 10;
    if(!tag.empty()){
        spec += 1;
    }
    return spec;
}

// ---------------------------------------------------------------
// StyleRule
// ---------------------------------------------------------------

StyleRule & StyleRule::setBackgroundColor(Composition::Color color){
    declarations[PropertyKey::BackgroundColor] = StyleValue{color};
    return *this;
}

StyleRule & StyleRule::setBorderColor(Composition::Color color){
    declarations[PropertyKey::BorderColor] = StyleValue{color};
    return *this;
}

StyleRule & StyleRule::setBorderWidth(std::uint32_t widthPx){
    declarations[PropertyKey::BorderWidth] = StyleValue{widthPx};
    return *this;
}

StyleRule & StyleRule::setFillBrush(SharedHandle<Composition::Brush> brush){
    declarations[PropertyKey::FillBrush] = StyleValue{std::move(brush)};
    return *this;
}

StyleRule & StyleRule::setDropShadow(Composition::LayerEffect::DropShadowParams params){
    declarations[PropertyKey::DropShadow] = StyleValue{std::move(params)};
    return *this;
}

StyleRule & StyleRule::setTextFont(SharedHandle<Composition::Font> font){
    declarations[PropertyKey::TextFont] = StyleValue{std::move(font)};
    return *this;
}

StyleRule & StyleRule::setTextColor(Composition::Color color){
    declarations[PropertyKey::TextColor] = StyleValue{color};
    return *this;
}

StyleRule & StyleRule::setTextLayout(Composition::TextLayoutDescriptor layout){
    declarations[PropertyKey::TextLayout] = StyleValue{std::move(layout)};
    return *this;
}

StyleRule & StyleRule::setTextLineLimit(std::uint32_t lineLimit){
    declarations[PropertyKey::TextLineLimit] = StyleValue{lineLimit};
    return *this;
}

bool StyleRule::beats(const StyleRule & other) const {
    // Mirrors `OmegaWTK::StyleRule::beats()` in `wtk/src/UI/Layout.cpp`:
    // higher specificity wins; tie-break by later `sourceOrder` so the
    // last-declared rule wins on a tie (the conventional CSS cascade
    // tie-break). This is intentionally `>=` on sourceOrder so a rule
    // beats itself — callers fold the comparison into a max-by-beats
    // walk and the equality case (same rule) preserves stability.
    const auto a = selector.specificity();
    const auto b = other.selector.specificity();
    if(a != b){
        return a > b;
    }
    return sourceOrder >= other.sourceOrder;
}

// ---------------------------------------------------------------
// StyleSheet
// ---------------------------------------------------------------

SharedHandle<StyleSheet> StyleSheet::Create(){
    // Default constructor — empty rule / keyframe sets. App code that
    // wants populated sheets goes through `Builder`.
    return SharedHandle<StyleSheet>(new StyleSheet());
}

const OmegaCommon::Vector<StyleRule> & StyleSheet::rules() const {
    return rules_;
}

const OmegaCommon::Map<OmegaCommon::String, KeyframeAnimation> &
StyleSheet::keyframeAnimations() const {
    return keyframes_;
}

// ---------------------------------------------------------------
// StyleSheet::Builder
// ---------------------------------------------------------------

StyleSheet::Builder & StyleSheet::Builder::addRule(StyleRule rule){
    // Stamp the source-order counter — the cascade comparator
    // (`StyleRule::beats`) uses this to break specificity ties so the
    // last-declared matching rule wins. The Builder counter is local
    // to one build session; the resolver does not need a globally
    // unique source order across sheets because the cascade walks
    // sheets in stack order and rules within a sheet by their own
    // `sourceOrder` field.
    rule.sourceOrder = nextSourceOrder_++;
    rules_.push_back(std::move(rule));
    return *this;
}

StyleSheet::Builder & StyleSheet::Builder::addKeyframeAnimation(
        KeyframeAnimation animation){
    // Name collisions overwrite — last declaration wins, matching
    // both the cascade `last-rule-wins` semantic and how CSS
    // `@keyframes <name>` redefinition behaves.
    keyframes_[animation.name] = std::move(animation);
    return *this;
}

SharedHandle<StyleSheet> StyleSheet::Builder::build() const {
    auto sheet = SharedHandle<StyleSheet>(new StyleSheet());
    sheet->rules_     = rules_;
    sheet->keyframes_ = keyframes_;
    return sheet;
}

// ---------------------------------------------------------------
// User-agent default stylesheet (D7.5, 2026-06-04)
// ---------------------------------------------------------------
//
// Seed defaults for the conventional text-element tags. Why only these
// two rules today:
//   * `Rectangle` / `RoundedRectangle` / `Ellipse` / `Path` /
//     `Separator` / `Image` already only author their inline `Style`
//     when the caller passed a non-null brush / image. The "default"
//     for an unauthored fill is "no cell written" — paint then skips
//     the fill draw entirely, which is the correct render today. A UA
//     rule that wrote a default fill would change that behavior; a UA
//     rule that wrote nothing is identical to the rule's absence.
//   * `Label` / `Icon` / `Button` author `textColor` (and a handful of
//     other text cells) unconditionally from the prop struct's
//     default-initialized `Color {0,0,0,1}`. The widget-strip
//     follow-up (deferred) will switch those props to optional cells
//     so an unset color falls through to the UA default. Until then
//     the rules below cover any APP-AUTHORED `UIView` that lays out a
//     text element without writing `textColor` inline — a real use
//     case today (custom widgets that share the conventional `label`
//     / `icon` element tags).
//
// Tier-1 selector machinery (D6.1) is single-compound — no descendant
// combinator — so the rules cannot be scoped to "a particular widget's
// element"; they match every element carrying the tag. That's fine
// here because the only readers of `textColor` in tree are text
// elements and the default is universally sensible.
SharedHandle<StyleSheet> BuildUserAgentStyleSheet(){
    StyleSheet::Builder builder;

    // Conventional `label` element default: black text.
    {
        StyleRule rule;
        rule.selector.tag = "label";
        rule.setTextColor(Composition::Color::create8Bit(
            Composition::Color::Black8));
        builder.addRule(std::move(rule));
    }

    // Conventional `icon` element default: black tint. The Icon widget
    // and Button's icon sub-element both render the token glyph via the
    // text path, so `TextColor` is the right cell.
    {
        StyleRule rule;
        rule.selector.tag = "icon";
        rule.setTextColor(Composition::Color::create8Bit(
            Composition::Color::Black8));
        builder.addRule(std::move(rule));
    }

    return builder.build();
}

} // namespace OmegaWTK::Style
