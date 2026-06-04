#include "omegaWTK/UI/StyleResolver.h"

#include "AnimationScheduler.h"
#include "UIViewImpl.h"
#include "FrameBuilder.h"
#include "omegaWTK/UI/App.h"
#include "omegaWTK/UI/AppWindow.h"
#include "omegaWTK/UI/ThemeVars.h"

#include <type_traits>
#include <unordered_set>
#include <utility>

namespace OmegaWTK::StyleSheets {

namespace {

/// Each `PropertyKey` cell lives at either the view's `NodeId`
/// (one cell per UIView) or the element's `NodeId` (one cell per
/// `UIElementTag` × property). The resolver writes accordingly
/// based on this scope map. Unmapped keys default to view scope —
/// the safer fall-through; D6 only ships view-level
/// `BackgroundColor` and the per-element pack from D5.
enum class PropertyScope : std::uint8_t {
    View,
    Element,
};

constexpr PropertyScope scopeOf(PropertyKey key){
    switch(key){
        case PropertyKey::FillBrush:
        case PropertyKey::DropShadow:
        case PropertyKey::TextFont:
        case PropertyKey::TextColor:
        case PropertyKey::TextLayout:
        case PropertyKey::TextLineLimit:
            return PropertyScope::Element;
        case PropertyKey::BackgroundColor:
        case PropertyKey::BorderColor:
        case PropertyKey::BorderWidth:
        case PropertyKey::Opacity:
        case PropertyKey::ShadowOffsetX:
        case PropertyKey::ShadowOffsetY:
        case PropertyKey::ShadowBlur:
        case PropertyKey::ShadowColor:
        case PropertyKey::TransformX:
        case PropertyKey::TransformY:
        case PropertyKey::TransformScaleX:
        case PropertyKey::TransformScaleY:
        case PropertyKey::TransformRotation:
        case PropertyKey::TextSize:
        case PropertyKey::LayoutWidth:
        case PropertyKey::LayoutHeight:
        case PropertyKey::LayoutX:
        case PropertyKey::LayoutY:
        case PropertyKey::PathNodeX:
        case PropertyKey::PathNodeY:
        case PropertyKey::UserDefined:
        default:
            return PropertyScope::View;
    }
}

/// Tier-1 selector match: tag + pseudo-class. id / class matching
/// is left to a follow-up tier (the View surface that carries id /
/// class authoring is still pending). Pseudo-class match is a
/// subset relation: every bit set in `sel.pseudoClasses` must be
/// set in the view's current state. `Selector::pseudoClasses ==
/// None` matches unconditionally — Tier-1 sheets that don't author
/// state constraints still match every view.
bool selectorMatches(const Selector & sel,
                     const OmegaCommon::String & candidateTag,
                     std::uint8_t viewPseudoBits){
    if(!sel.tag.empty() && sel.tag != candidateTag){
        return false;
    }
    if(!sel.id.empty()){
        // id constraint set but no node-id surface yet — refuse.
        return false;
    }
    if(!sel.classes.empty()){
        // class constraint set but no class surface yet — refuse.
        return false;
    }
    const auto reqBits = static_cast<std::uint8_t>(sel.pseudoClasses);
    if((viewPseudoBits & reqBits) != reqBits){
        return false;
    }
    return true;
}

/// Apply each declaration of `rule` to `view` via `applyOne`. The
/// callback is invoked once per property in the rule's
/// `declarations` map. `subIndex` is always 0 in D6 — keyframe /
/// path-node sub-cells route through the scheduler, not the sheet
/// cascade.
template<typename Apply>
void forEachDeclaration(const StyleRule & rule, Apply && applyOne){
    for(const auto & [key, value] : rule.declarations){
        applyOne(key, value);
    }
}

/// Widget-View-Paint-Lifecycle-Plan Tier D / D7.1 (2026-06-04):
/// substitute a sheet `StyleValue` against the active `ThemeVars`.
///   * Non-`Var` values pass through unchanged.
///   * A `Var` whose name is bound in the theme returns the bound
///     `StyleValue`.
///   * A `Var` is treated as unbound — and substituted with
///     `monostate` (which the cell-write loop skips) — when any of
///     the following hold:
///       (a) No `ThemeVars` is installed on `AppInst`.
///       (b) The theme has no entry for the name.
///       (c) The bound entry is itself a `Var`. Chains are NOT
///           followed in D7.1; the layered design assumes themes
///           hold concrete values. Chain support is a follow-up if
///           a real use case shows up.
///       (d) The bound entry's slot is `monostate` (a defensive case
///           for a `ThemeVars::Builder::set(name, StyleValue{})`).
///
/// Producing `monostate` for the unresolved cases is what makes the
/// "inline `Style` writes overwrite on cell overlap" layering still
/// give a clean fallthrough: if no Var was bound, the resolver writes
/// nothing for that cell, and the inline-Style pass that runs after
/// the resolver still gets the chance to author the property.
StyleValue resolveVar(const StyleValue & value, const ThemeVars * theme){
    const auto * varRef = std::get_if<Var>(&value);
    if(varRef == nullptr){
        return value;
    }
    if(theme == nullptr){
        return StyleValue{std::monostate{}};
    }
    auto bound = theme->lookup(varRef->name);
    if(!bound.has_value()){
        return StyleValue{std::monostate{}};
    }
    // (c) chain rejection + (d) monostate passthrough.
    if(std::holds_alternative<Var>(*bound) ||
       std::holds_alternative<std::monostate>(*bound)){
        return StyleValue{std::monostate{}};
    }
    return *bound;
}

} // namespace

void StyleResolver::apply(UIView & view){
    auto * fb = AppWindow::activeFrameBuilder();
    if(fb == nullptr){
        // No active FrameBuilder means no AppWindow context — the
        // inline-style path still runs in resolveStyles() unchanged.
        return;
    }
    const auto & stack = fb->window().styleSheets();
    if(stack.empty()){
        return;
    }

    auto & impl       = *view.impl_;
    const auto viewTag = impl.tag;
    const auto viewNodeId = view.nodeId();
    // Tier D / D6.4 (2026-06-03): pseudo-class state for selector
    // matching. Element-level rules currently inherit the view's
    // state — per-element state surfaces would need each element to
    // carry its own bitmask (not in tree). D6 keeps the simpler
    // shape; element pseudo-classes are a follow-up.
    const auto pseudoBits = view.pseudoClassBits();

    // Winners table — one entry per (NodeId, PropertyKey) cell the
    // cascade resolves. Tracks the best matching rule so the inline-
    // style writes that follow this resolver can blow it away (per
    // §0.3 layering decision: inline wins).
    struct CellWinner {
        const StyleRule * rule       = nullptr;
        StyleValue        value      {};
    };
    std::unordered_map<PropertyTableKey, CellWinner,
                       PropertyTableKeyHash> winners;

    auto consider = [&](NodeId node, PropertyKey key,
                        const StyleValue & value,
                        const StyleRule & rule){
        const PropertyTableKey cellKey{node, key, 0};
        auto it = winners.find(cellKey);
        if(it == winners.end() || rule.beats(*it->second.rule)){
            winners[cellKey] = CellWinner{&rule, value};
        }
    };

    // Walk the sheet stack and collect winners. Outer index is
    // sheet-stack position; tie-break inside a sheet uses each
    // rule's `sourceOrder` via `StyleRule::beats`. Sheets later in
    // the stack don't get an implicit specificity boost — they only
    // win ties through `sourceOrder` because `Builder::addRule`
    // stamps source order monotonically inside a single sheet, and
    // across sheets the cascade is whatever specificity says.
    for(const auto & sheet : stack){
        if(sheet == nullptr){
            continue;
        }
        for(const auto & rule : sheet->rules()){
            const bool viewMatches  = selectorMatches(rule.selector, viewTag, pseudoBits);

            if(viewMatches){
                forEachDeclaration(rule,
                                   [&](PropertyKey k, const StyleValue & v){
                    if(scopeOf(k) == PropertyScope::View){
                        consider(viewNodeId, k, v, rule);
                    }
                });
            }

            // Element matches: each UIElementTag is matched
            // independently. Element-scope properties land on the
            // element's NodeId (lazy-allocated via ensureElementNodeId).
            for(const auto & spec : impl.currentLayoutV2_.elements()){
                if(!selectorMatches(rule.selector, spec.tag, pseudoBits)){
                    continue;
                }
                const auto elNode = impl.ensureElementNodeId(spec.tag);
                forEachDeclaration(rule,
                                   [&](PropertyKey k, const StyleValue & v){
                    if(scopeOf(k) == PropertyScope::Element){
                        consider(elNode, k, v, rule);
                    }
                });
            }
        }
    }

    // Apply winners to the style table. Inline `Style` writes
    // happen AFTER this returns (per §0.3 layering); on cell
    // overlap, inline overwrites.

    // Widget-View-Paint-Lifecycle-Plan Tier D / D6.5 (2026-06-03):
    // RECORD transition specs + keyframe-animation bindings carried
    // by winning rules. D7 reads these to fire the scheduler — D6.5
    // is wiring + recording only, no firing.
    impl.sheetBindings_.clear();
    {
        // For each winner, record any TransitionSpec entries the
        // winning rule declares. Transitions are recorded per
        // (NodeId, PropertyKey from spec.key) — the spec's key may
        // or may not be the cell key; that's intentional CSS-like
        // semantics (a rule transitioning a property it doesn't
        // declare).
        std::unordered_set<const StyleRule *> winningRules;
        for(const auto & entry : winners){
            winningRules.insert(entry.second.rule);
        }
        for(const auto * rule : winningRules){
            if(rule == nullptr){
                continue;
            }
            // A winning rule contributes its transitions against
            // whichever node it won. Two-pass: find every (node, key)
            // the rule won, attach the rule's transitions to those
            // nodes. D7 resolves which spec.key the transition fires
            // for by walking `transitions` and checking the per-cell
            // previous value.
            std::unordered_set<NodeId> nodesWonByRule;
            for(const auto & entry : winners){
                if(entry.second.rule == rule){
                    nodesWonByRule.insert(entry.first.node);
                }
            }
            for(NodeId node : nodesWonByRule){
                for(const auto & spec : rule->transitions){
                    impl.sheetBindings_.transitions.push_back(
                        ResolvedSheetBindings::TransitionRecord{node, spec});
                }
                if(rule->animationName){
                    impl.sheetBindings_.animationBindings.push_back(
                        ResolvedSheetBindings::AnimationBindingRecord{
                            node, *rule->animationName});
                }
            }
        }
    }
    // Widget-View-Paint-Lifecycle-Plan Tier D / D7.1 (2026-06-04):
    // resolve any sheet-side `Var{name}` against the active
    // `ThemeVars` BEFORE writing into the per-property style table.
    // The table holds *concrete* values only — by the time Paint
    // reads `resolved<T>()`, every Var must already be replaced (or
    // dropped). Reading `AppInst::inst()` once outside the loop is
    // safe: theme swaps go through `setThemeVars` which dirties this
    // window's cascade and runs a fresh `apply()` pass; mid-pass
    // mutations are not part of D7.1's contract.
    const ThemeVars * theme = nullptr;
    if(auto * appInst = AppInst::inst(); appInst != nullptr){
        theme = appInst->themeVars().get();
    }

    for(const auto & entry : winners){
        // Pre-extract from the map entry — capturing structured
        // bindings in a lambda is C++20, this repo is C++17 (see
        // AGENTS.md).
        const auto cellKey  = entry.first;
        const auto resolved = resolveVar(entry.second.value, theme);
        // Type-erased visit — write whatever variant alternative the
        // rule declared. The `std::monostate` slot is treated as "no
        // value" and skipped (a sheet rule declaring a property with
        // monostate is equivalent to omitting it, and an unresolved
        // `Var` arrives here as monostate via `resolveVar`).
        // `Var` itself is also skipped defensively — `resolveVar`
        // should have removed every Var, but writing a `Var` into the
        // runtime style table would break Paint's `resolved<T>` calls.
        std::visit([&](const auto & val){
            using T = std::decay_t<decltype(val)>;
            if constexpr (!std::is_same_v<T, std::monostate> &&
                          !std::is_same_v<T, Var>){
                impl.styleTable_.set<T>(cellKey.node, cellKey.key, val,
                                        cellKey.subIndex);
            }
        }, resolved);
    }
}

namespace {

/// Widget-View-Paint-Lifecycle-Plan Tier D / D7.2 (2026-06-04):
/// Compile-time predicate naming the `StyleValue` alternatives that
/// have a meaningful animated form in the scheduler's `AnimatedValue`
/// variant AND a `KeyframeLerp<T>` specialization (or arithmetic
/// fallback). The transitionable set in D7.2:
///   * `Composition::Color` — D7.2 adds the lerp specialization.
///   * `std::uint32_t` — arithmetic fallback (lerps as int).
///   * `Composition::LayerEffect::DropShadowParams` — pre-existing lerp.
/// Excluded:
///   * `SharedHandle<Composition::Brush>` — brush handles are
///     identities, not interpolable; a brush swap is a discrete
///     change. Snap is the correct CSS-like behavior. The
///     scheduler's `AnimatedValue` variant carries Brush only for
///     code-driven `tweenProperty` paths that the existing tree
///     does not actually exercise; D7.2 treats brush transitions
///     as snap.
///   * `Font`, `TextLayoutDescriptor`, `monostate`, `Var` — not
///     in `AnimatedValue` at all; CSS treats these as not
///     animatable.
template<typename T>
constexpr bool isTransitionable_v =
    std::is_same_v<T, Composition::Color> ||
    std::is_same_v<T, std::uint32_t>      ||
    std::is_same_v<T, Composition::LayerEffect::DropShadowParams>;

/// Tier-1 equality for the transitionable subset. `Brush` handle
/// equality compares the underlying `shared_ptr` (identity, not
/// brush-content equivalence); a brush swap is therefore a
/// transition trigger even if the two brushes paint the same.
/// `DropShadowParams` uses member-wise compare via its own
/// `operator==` if defined; otherwise the compiler will reject the
/// template instantiation and we'll surface the case here. For
/// simplicity and to match CSS shadow semantics, we treat a
/// `DropShadowParams` change as any field change.
template<typename T>
bool valuesEqual(const T & a, const T & b);

template<> bool valuesEqual<Composition::Color>(
        const Composition::Color & a, const Composition::Color & b){
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}
template<> bool valuesEqual<std::uint32_t>(
        const std::uint32_t & a, const std::uint32_t & b){
    return a == b;
}
template<> bool valuesEqual<Composition::LayerEffect::DropShadowParams>(
        const Composition::LayerEffect::DropShadowParams & a,
        const Composition::LayerEffect::DropShadowParams & b){
    // Field-wise compare. `DropShadowParams` does not define
    // `operator==` in tree, so we spell it out here. If a field is
    // added later, this comparison stays correct only if the new
    // field is added below — the test for that is "two shadows that
    // differ only in the new field should retrigger the transition."
    // `Composition::Color::operator==` is a non-const-receiver member
    // (Brush.h:23), so we cannot call it on const references —
    // compare RGBA component-wise instead.
    return a.x_offset   == b.x_offset   && a.y_offset == b.y_offset &&
           a.radius     == b.radius     &&
           a.blurAmount == b.blurAmount &&
           a.opacity    == b.opacity    &&
           a.color.r    == b.color.r    && a.color.g  == b.color.g  &&
           a.color.b    == b.color.b    && a.color.a  == b.color.a;
}

} // namespace

void StyleResolver::applyTransitions(UIView & view){
    auto & impl = *view.impl_;
    if(impl.sheetBindings_.transitions.empty()){
        return;
    }
    auto * fb = AppWindow::activeFrameBuilder();
    if(fb == nullptr){
        return;
    }
    auto * scheduler = fb->animationScheduler();
    if(scheduler == nullptr){
        return;
    }

    // One transition firing per recorded (node, spec). A rule that
    // declared `transition: BackgroundColor 200ms` against multiple
    // matching nodes shows up as one record per node — each fires
    // independently against its own (node, key) cell.
    for(const auto & record : impl.sheetBindings_.transitions){
        const auto    node = record.node;
        const auto &  spec = record.spec;
        const auto    key  = spec.key;

        // Pull raw cells from previous and current style tables. A
        // missing `prev` (first frame or first appearance of this
        // node × key) means we have no value to transition FROM, so
        // we snap to the new value — Paint already reads `styleTable_`.
        const StyleValue * prevCell = impl.previousStyleTable_.getRaw(node, key);
        const StyleValue * currCell = impl.styleTable_.getRaw(node, key);
        if(prevCell == nullptr || currCell == nullptr){
            continue;
        }

        // Dispatch on the previous-cell variant: the new cell must
        // hold the same alternative for a transition to make sense
        // (a property switching variant types is a hard change, not
        // a smooth interpolation — snap).
        std::visit([&](const auto & prevVal){
            using T = std::decay_t<decltype(prevVal)>;
            if constexpr (isTransitionable_v<T>){
                if(const T * currVal = std::get_if<T>(currCell)){
                    if(!valuesEqual<T>(prevVal, *currVal)){
                        scheduler->transition<T>(node, key, prevVal, *currVal, spec);
                    }
                }
            }
        }, *prevCell);
    }
}

} // namespace OmegaWTK::StyleSheets
