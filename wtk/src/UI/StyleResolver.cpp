#include "omegaWTK/UI/StyleResolver.h"

#include "UIViewImpl.h"
#include "FrameBuilder.h"
#include "omegaWTK/UI/AppWindow.h"

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
    for(const auto & entry : winners){
        // Pre-extract from the map entry — capturing structured
        // bindings in a lambda is C++20, this repo is C++17 (see
        // AGENTS.md).
        const auto cellKey = entry.first;
        const auto & value = entry.second.value;
        // Type-erased visit — write whatever variant alternative the
        // rule declared. The `std::monostate` slot is treated as "no
        // value" and skipped (a sheet rule declaring a property with
        // monostate is equivalent to omitting it).
        std::visit([&](const auto & val){
            using T = std::decay_t<decltype(val)>;
            if constexpr (!std::is_same_v<T, std::monostate>){
                impl.styleTable_.set<T>(cellKey.node, cellKey.key, val,
                                        cellKey.subIndex);
            }
        }, value);
    }
}

} // namespace OmegaWTK::StyleSheets
