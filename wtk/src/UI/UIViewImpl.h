#ifndef OMEGAWTK_UI_UIVIEWIMPL_H
#define OMEGAWTK_UI_UIVIEWIMPL_H

#include "omegaWTK/UI/UIView.h"
#include "omegaWTK/UI/StyleSheet.h"  // D6.1: TransitionSpec, KeyframeAnimation
#include "omegaWTK/Composition/TextLayoutEngine.h"  // Text-Measurement-API-Plan: cached LayoutResult shared by measure + paint.
#include "AnimationScheduler.h"   // Phase 4.4: NodeId, PropertyKey, scheduler API.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <type_traits>
#include <unordered_map>
#include <variant>

namespace OmegaWTK {

namespace UIViewInternalDetail {

// Widget-View-Paint-Lifecycle-Plan Tier D / D5 (2026-06-03):
// compile-time membership check for `std::variant`. Used to gate the
// animation-scheduler probe inside `Impl::resolved<T>` — querying
// `AnimationScheduler::value<T>` with a `T` that isn't in
// `AnimatedValue` triggers a `std::get_if` static_assert at the
// libc++ tuple machinery, so we have to skip the probe at compile
// time for non-animatable types (Font handle, TextLayoutDescriptor)
// rather than relying on the scheduler returning nullopt at runtime.
template<typename T, typename Variant>
struct VariantHas : std::false_type {};

template<typename T, typename... Ts>
struct VariantHas<T, std::variant<Ts...>> :
    std::disjunction<std::is_same<T, Ts>...> {};

template<typename T, typename Variant>
inline constexpr bool VariantHas_v = VariantHas<T, Variant>::value;

}

// Widget-View-Paint-Lifecycle-Plan Tier D / D6.1 (2026-06-03):
// `StyleValue` (D5) lifted to the public header
// `omegaWTK/UI/StyleProperty.h` so the new sheet vocabulary can
// share the same cell-value union. Included transitively via
// `AnimationScheduler.h`.

/// Widget-View-Paint-Lifecycle-Plan Tier D / D6.5 (2026-06-03):
/// per-node records the cascade walker emits so D7 has somewhere
/// to read transition / keyframe-animation bindings from when it
/// wires the firing. The resolver clears + repopulates these every
/// Style pass; D7.2 / D7.3 read them to call
/// `scheduler.transition(...)` / `scheduler.animateProperty(...)`.
/// Tier D / D6.5 only RECORDS — there is no firing path on this
/// data yet; the records are inert until D7 grows readers.
struct ResolvedSheetBindings {
    struct TransitionRecord {
        NodeId                       node = 0;
        StyleSheets::TransitionSpec  spec {};
    };
    struct AnimationBindingRecord {
        NodeId                       node = 0;
        OmegaCommon::String          name {};
    };
    OmegaCommon::Vector<TransitionRecord>          transitions {};
    OmegaCommon::Vector<AnimationBindingRecord>    animationBindings {};

    void clear() {
        transitions.clear();
        animationBindings.clear();
    }
};

/// Widget-View-Paint-Lifecycle-Plan Tier D / D7.3 (2026-06-04):
/// per-node tracking of in-flight keyframe-animation bindings.
/// `StyleResolver::applyKeyframeBindings` looks up an entry per
/// (node, name) pair to decide whether to start, leave alone, or
/// cancel-and-replace a sheet-driven `animation: <name>` binding.
/// `handles` carries one `AnimationHandle` per property the named
/// `KeyframeAnimation` declares — cancelled together when the
/// binding stops matching or the name changes.
struct ActiveKeyframeBinding {
    OmegaCommon::String                              name {};
    OmegaCommon::Vector<Composition::AnimationHandle> handles {};
};

/// Widget-View-Paint-Lifecycle-Plan Tier D / D5 (2026-06-03):
/// per-property resolved-style table. Owns one cell per resolved
/// property per node; written by `UIView::resolveStyles()` and read
/// from Paint via `UIView::Impl::resolved<T>`. Keyed by the
/// scheduler's `PropertyTableKey` shape so the animation side table
/// and the style side table can be queried interchangeably by the
/// same `(NodeId, PropertyKey, subIndex)` triple.
class StyleTable {
public:
    void clear() { cells_.clear(); }
    void swap(StyleTable & other) noexcept { cells_.swap(other.cells_); }

    template<typename T>
    void set(NodeId node, PropertyKey key, T value, std::uint32_t subIndex = 0){
        cells_[PropertyTableKey{node, key, subIndex}] = StyleValue{std::move(value)};
    }

    template<typename T>
    Core::Optional<T> get(NodeId node, PropertyKey key, std::uint32_t subIndex = 0) const {
        auto it = cells_.find(PropertyTableKey{node, key, subIndex});
        if(it == cells_.end()){
            return {};
        }
        if(const T * typed = std::get_if<T>(&it->second)){
            return *typed;
        }
        return {};
    }

    /// Widget-View-Paint-Lifecycle-Plan Tier D / D7.2 (2026-06-04):
    /// Return the raw `StyleValue` cell at `(node, key, subIndex)`,
    /// or `nullptr` if the cell does not exist. Used by the
    /// transition-firing pass in `StyleResolver::applyTransitions`
    /// to compare previous vs. current values across all variant
    /// alternatives without committing to a single `T` up front —
    /// the visit lambda dispatches on the variant.
    const StyleValue * getRaw(NodeId node, PropertyKey key,
                              std::uint32_t subIndex = 0) const {
        auto it = cells_.find(PropertyTableKey{node, key, subIndex});
        return (it != cells_.end()) ? &it->second : nullptr;
    }

private:
    std::unordered_map<PropertyTableKey, StyleValue, PropertyTableKeyHash> cells_;
};

namespace UIViewInternal {

struct ResolvedViewStyle {
    Core::Optional<Composition::Color> backgroundColor {};
    bool useBorder = false;
    Core::Optional<Composition::Color> borderColor {};
    float borderWidth = 0.f;
};

struct ResolvedEffectTransition {
    bool transition = false;
    float duration = 0.f;
    SharedHandle<Composition::AnimationCurve> curve = nullptr;
};

struct ResolvedTextStyle {
    SharedHandle<Composition::Font> font = nullptr;
    Composition::Color color = Composition::Color::create8Bit(Composition::Color::Black8);
    Composition::TextLayoutDescriptor layout {
        Composition::TextLayoutDescriptor::LeftUpper,
        Composition::TextLayoutDescriptor::None
    };
    unsigned lineLimit = 0;
    // D7.5b: inline transition metadata for the winning `TextColor`
    // entry, surfaced so `resolveStyles` can fire an inline-authored
    // text-color transition. Only the color cell animates today.
    ResolvedEffectTransition colorTransition {};
    // Widget-Inline-Default-Strip-Plan Phase L (2026-07-01): per-field
    // presence flags. `resolveTextStyle` fills the `color`/`layout`/
    // `lineLimit` fields with defaults when the inline `Style` did not
    // author them; these flags let `resolveStyles` tell "authored" apart
    // from "defaulted" so an unauthored cell is left for the UA-sheet
    // cascade instead of being overwritten by the default. (Font presence
    // is already signalled by `font != nullptr`.) Alignment and wrapping
    // are tracked separately even though they share one fused `TextLayout`
    // cell, so a Style that authors only one sub-field merges over the
    // sheet-resolved layout rather than clobbering the other sub-field.
    bool hasColor = false;
    bool hasAlignment = false;
    bool hasWrapping = false;
    bool hasLineLimit = false;
};

struct ResolvedEffectStyle {
    Core::Optional<Composition::LayerEffect::DropShadowParams> dropShadow {};
    ResolvedEffectTransition dropShadowTransition {};
    Core::Optional<Composition::CanvasEffect::GaussianBlurParams> gaussianBlur {};
    ResolvedEffectTransition gaussianBlurTransition {};
    Core::Optional<Composition::CanvasEffect::DirectionalBlurParams> directionalBlur {};
    ResolvedEffectTransition directionalBlurTransition {};
};

// Element-scoped stroke outline authored via Style::elementBorder. Both
// fields are optional so a Style that sets only one is a partial override;
// paint draws a border only when both a color and a positive width
// resolve.
struct ResolvedElementBorder {
    Core::Optional<Composition::Color> color {};
    Core::Optional<float> width {};
};

// Widget-View-Paint-Lifecycle-Plan Tier D / D5 (2026-06-03):
// `ComputedStyle` aggregate deleted. Pre-D5 it was the per-element
// resolved-style triple `{brush, effects, text}` stored in
// `Impl::computedStyles_` and read via `computedStyleFor(tag)`. The
// resolved-style cache is now the per-property `StyleTable` (one
// cell per resolved property per element, keyed by
// `(NodeId, PropertyKey, subIndex)`), and Paint reads via
// `Impl::resolved<T>(node, key, fallback)`. The transient builder
// types `ResolvedViewStyle` / `ResolvedTextStyle` /
// `ResolvedEffectStyle` survive as the return shapes of the
// `resolve*Style()` helpers used inside `UIView::resolveStyles()`;
// only the aggregate that *stored* their union as a Map value
// disappears.

struct StyleScope {
    bool touchesRoot = false;
    bool touchesAllElements = false;
    OmegaCommon::Vector<UIElementTag> elementTags {};
};

// Tier B / B3: one element's resolved layout — the output of the Layout
// phase (arrange()), consumed by the Paint phase (paint()). `spec` points
// into `currentLayoutV2_.elements()` and is valid only within a single
// update() (the layout vector is not mutated between arrange and paint).
struct ArrangedElement {
    UIElementTag tag {};
    const UIElementLayoutSpec * spec = nullptr;
    Composition::Rect resolvedRectDp {};
    Composition::Rect resolvedRectPx {};
    int zIndex = 0;
    std::size_t insertionOrder = 0;
};

ResolvedViewStyle resolveViewStyle(const StylePtr & style,const UIViewTag & viewTag);
SharedHandle<Composition::Brush> resolveElementBrush(const StylePtr & style,
                                                     const UIViewTag & viewTag,
                                                     const UIElementTag & elementTag,
                                                     ResolvedEffectTransition * outTransition = nullptr);
ResolvedTextStyle resolveTextStyle(const StylePtr & style,
                                   const UIViewTag & viewTag,
                                   const UIElementTag & elementTag);
ResolvedEffectStyle resolveElementEffectStyle(const StylePtr & style,
                                              const UIViewTag & viewTag,
                                              const UIElementTag & elementTag);
ResolvedElementBorder resolveElementBorder(const StylePtr & style,
                                           const UIViewTag & viewTag,
                                           const UIElementTag & elementTag);
bool containsTag(const OmegaCommon::Vector<UIElementTag> & tags,const UIElementTag & tag);
void addUniqueTag(OmegaCommon::Vector<UIElementTag> & tags,const UIElementTag & tag);
StyleScope collectStyleScope(const StylePtr & style,const UIViewTag & viewTag);
Composition::Rect localBoundsFromView(UIView * view);

}

struct UIView::Impl {
    struct ElementDirtyState {
        bool layoutDirty = true;
        bool styleDirty = true;
        bool contentDirty = true;
        bool orderDirty = true;
        bool visibilityDirty = true;
    };

    // Widget-View-Paint-Lifecycle-Plan Tier D / D4 (2026-06-03):
    // `PropertyAnimationState` + `PathNodeAnimationState` deleted —
    // both were Phase-4.4-vintage tween-bookkeeping carcasses with
    // no `.cpp` reader after the scheduler took over the tween pump.
    //
    // The `EffectAnimationKey*` enum kept: `UIView.Update.cpp:244–
    // 266` reads the Shadow* nine values via `animatedValue(tag,
    // key)` to merge animated shadow channels into the resolved
    // effect state. The trailing three (`GaussianRadius`,
    // `DirectionalRadius`, `DirectionalAngle`) had zero in-tree
    // readers and are trimmed below.
    enum : int {
        EffectAnimationKeyShadowOffsetX = 1000,
        EffectAnimationKeyShadowOffsetY,
        EffectAnimationKeyShadowRadius,
        EffectAnimationKeyShadowBlur,
        EffectAnimationKeyShadowOpacity,
        EffectAnimationKeyShadowColorR,
        EffectAnimationKeyShadowColorG,
        EffectAnimationKeyShadowColorB,
        EffectAnimationKeyShadowColorA
    };

    explicit Impl(UIView & ownerRef,UIViewTag tagValue);

    UIView & owner;
    int framesPerSec = 60;
    UIViewTag tag;
    UIViewLayout currentLayout;
    StylePtr currentStyle;
    bool layoutDirty = true;
    bool styleDirty = true;
    bool firstFrameCoherentSubmit = true;
    bool styleDirtyGlobal = false;
    bool styleChangeRequiresCoherentFrame = false;
    // Phase 4.8: the dormant per-tag tween bookkeeping (Phase 4.4
    // recorded as dormant; nothing wrote these after the
    // `startOrUpdateAnimation` routing moved to the
    // `AnimationScheduler`) is gone. The four ViewAnimator /
    // LayerAnimator / elementAnimations / pathNodeAnimations members
    // and their `ensure*` / `beginCompositionClock` helpers are
    // deleted alongside the underlying classes in
    // `Composition/Animation.{h,cpp}`.
    OmegaCommon::Map<UIElementTag,ElementDirtyState> elementDirtyState;
    OmegaCommon::Vector<UIElementTag> activeTagOrder;
    // Phase 4.4 (Anim Tier C): stable NodeId per element tag, allocated on
    // first animation reference (registration or read). The
    // AnimationScheduler keys its side table on
    // `(elementNodeId, PropertyKey, subIndex)`; `applyAnimated*` /
    // `animatedValue` look up the NodeId before probing the scheduler.
    OmegaCommon::Map<UIElementTag,NodeId> elementNodeIds_;
    // Phase 4.4 (Anim Tier C): preserves the legacy "same target = no
    // restart" short-circuit on `startOrUpdateAnimation`. The scheduler
    // itself replaces on every re-registration (Anim §6 Q3); this map
    // tracks the last requested `.to` per `(tag, key)` so a repeated call
    // with the same target is a no-op rather than a fresh tween.
    OmegaCommon::Map<UIElementTag,OmegaCommon::Map<int,float>> animationTargets_;
    // Widget-View-Paint-Lifecycle-Plan Tier D / D4 (2026-06-03):
    // sibling of `animationTargets_` for the path-node sub-cell
    // surface. Outer key is the element tag, inner key packs
    // `(axisKey, nodeIndex)` into a 64-bit slot so two different
    // node indices don't collide on the same axis. Same to-match
    // short-circuit semantics — repeated calls with the same `(tag,
    // axis, nodeIndex, to)` are a no-op.
    OmegaCommon::Map<UIElementTag,OmegaCommon::Map<std::uint64_t,float>> pathNodeAnimationTargets_;
    OmegaCommon::Map<UIElementTag,Composition::Color> lastResolvedElementColor;
    OmegaCommon::Map<UIElementTag,EffectState> lastResolvedEffects;
    OmegaCommon::Map<UIElementTag,Shape> previousShapeByTag;
    SharedHandle<Composition::Font> fallbackTextFont = nullptr;
    UIViewLayoutV2 currentLayoutV2_;
    OmegaCommon::Map<UIElementTag,Composition::Rect> lastResolvedV2Rects_;

    // Text-Measurement-API-Plan §3 / §5 (measure+paint unification): per-
    // element cache of the *full* laid-out text result, keyed by the
    // available width it was laid out against. `ensureTextLayout()` runs
    // `TextLayoutEngine::layout` only on a miss (new width, or a content /
    // style edit cleared the cache); both the measure pass (reads
    // `layoutHeight`) and the paint pass (consumes `glyphs`) share the one
    // cached result, so the text is laid out once per actual change rather
    // than once per frame AND measure can never disagree with paint about
    // line count or wrapping. The cached layout is computed top-origin
    // (vertical alignment neutralized to the Upper variant) so it is
    // rect.h-independent; paint applies the box's vertical offset itself.
    // Invalidation: `resolveStyles()` clears this whenever the Style phase
    // runs (every Label edit funnels through `setStyle`, setting the Style
    // dirty bit); a bare resize that does not dirty Style is caught by the
    // per-entry width compare in `ensureTextLayout`.
    struct TextLayoutCacheEntry {
        float availWidthDp = -1.f;   // -1 == no cached layout yet
        Composition::LayoutResult layout {};
    };
    OmegaCommon::Map<UIElementTag,TextLayoutCacheEntry> textLayoutCache_;

    // Resolve the tagged text element's font / descriptor / line limit
    // exactly as paint does, lay it out once at `availWidthDp` (top-origin),
    // and cache the result keyed by width. Returns a pointer into the cache
    // (stable until the cache is cleared/reassigned), or nullptr when the
    // tag has no text element or no font / shaper is available.
    const Composition::LayoutResult * ensureTextLayout(const UIElementTag & tag,
                                                       float availWidthDp);

    // Widget-View-Paint-Lifecycle-Plan Tier D / D5 (2026-06-03):
    // resolved-style cache, re-shaped from the pre-D5 aggregates
    // (`resolvedViewStyle_` + per-element `computedStyles_`) to one
    // unified per-property table keyed by `(NodeId, PropertyKey,
    // subIndex)` — the same shape the AnimationScheduler side-table
    // uses. `resolveStyles()` writes cells per resolved property
    // per element (the view-level cells use `View::nodeId()`); Paint
    // reads via `resolved<T>(node, key, fallback)` which chains
    // scheduler → style table → UA default. The aggregate
    // `ResolvedViewStyle` / `ResolvedTextStyle` / `ResolvedEffectStyle`
    // / `ComputedStyle` structs remain as transient builder types
    // inside `resolveStyles()`; they are no longer stored fields.
    StyleTable styleTable_ {};

    // Widget-View-Paint-Lifecycle-Plan Tier D / D7.2 (2026-06-04):
    // snapshot of the previous Style phase's `styleTable_`. The
    // transition-firing pass in `StyleResolver::applyTransitions`
    // compares this snapshot to the freshly resolved `styleTable_`
    // to detect cell-level changes that should fire a sheet-
    // declared transition. The snapshot is refreshed by swapping
    // (not copying) at the top of `UIView::resolveStyles` — the new
    // empty `styleTable_` is then populated by the resolver and the
    // inline-style writes that follow, and the swap leaves the
    // previous frame's content here for comparison.
    StyleTable previousStyleTable_ {};

    // Widget-View-Paint-Lifecycle-Plan Tier D / D6.5 (2026-06-03):
    // recorded transition / keyframe-animation bindings from the
    // sheet cascade. `StyleSheets::StyleResolver::apply()` clears
    // and repopulates this every Style pass. D7.2 reads
    // `sheetBindings_.transitions` to fire `scheduler.transition(...)`;
    // D7.3 reads `animationBindings` to fire / cancel
    // `scheduler.animateProperty<AnimatedValue>(...)` per property
    // of the named animation. Tier D / D6 only writes; both
    // readers landed in D7 (2026-06-04).
    ResolvedSheetBindings sheetBindings_ {};

    // Widget-View-Paint-Lifecycle-Plan Tier D / D7.3 (2026-06-04):
    // in-flight keyframe bindings, keyed by node. Each entry tracks
    // the binding's name + the per-property `AnimationHandle`s the
    // resolver started. `StyleResolver::applyKeyframeBindings`
    // reconciles this map against `sheetBindings_.animationBindings`
    // every Style pass — cancels handles for removed / renamed
    // bindings and starts handles for new ones. Same-name re-
    // application is a no-op (the running animation is preserved).
    std::unordered_map<NodeId, ActiveKeyframeBinding> activeKeyframeBindings_;

    // Tier B / B3: arranged layout (the Layout phase's output). `arrange()`
    // writes both; `paint()` reads them. Rebuilt every frame for now.
    OmegaCommon::Vector<UIViewInternal::ArrangedElement> arranged_;
    Composition::Rect arrangedLocalBounds_ {};
    LayoutDiagnosticSink * diagnosticSink_ = nullptr;
    UpdateDiagnostics lastUpdateDiagnostics {};
    // Widget-View-Paint-Lifecycle-Plan Tier D / D8 (2026-06-04):
    // `AnimationDiagnostics lastAnimationDiagnostics`,
    // `lastObservedDroppedPacketCount`, `hasObservedLaneDiagnostics`
    // deleted — all three carried per-lane packet bookkeeping for
    // the pre-scheduler `ViewAnimator` runtime that retired in
    // Phase 4.8. No writer survived the cutover, and no reader
    // survived the diagnostic-API retirement above.

    void markAllElementsDirty();
    void markElementDirty(const UIElementTag & tag,
                          bool layout,
                          bool style,
                          bool content,
                          bool order,
                          bool visibility);
    bool isElementDirty(const UIElementTag & tag) const;
    void clearElementDirty(const UIElementTag & tag);
    // Phase 4.8: `ensureAnimationViewAnimator` / `ensureAnimationLayerAnimator`
    // / `beginCompositionClock` deleted alongside the dormant
    // ViewAnimator / LayerAnimator classes they spawned. The live
    // animation pipe runs `startOrUpdateAnimation` →
    // `AnimationScheduler` directly.
    void startOrUpdateAnimation(const UIElementTag & tag,
                                int key,
                                float from,
                                float to,
                                float durationSec,
                                SharedHandle<Composition::AnimationCurve> curve);
    // Widget-View-Paint-Lifecycle-Plan Tier D / D4 (2026-06-03):
    // path-node sibling. Keys the scheduler side-table cell by
    // `(ensureElementNodeId(tag), PropertyKey::PathNodeX/Y,
    // subIndex=nodeIndex)` via `tweenPropertyAt<float>`, so two
    // different node indices on the same element animate
    // independently. `axisKey` is one of
    // `ElementAnimationKeyPathNodeX/Y` — same `int` mapping the
    // public `UIView::PathNodeAxis` casts to. Maintains the same
    // `(tag, axis, nodeIndex, to)` to-match short-circuit semantics
    // as `startOrUpdateAnimation`, via `pathNodeAnimationTargets_`.
    void startOrUpdatePathNodeAnimation(const UIElementTag & tag,
                                        int axisKey,
                                        int nodeIndex,
                                        float from,
                                        float to,
                                        float durationSec,
                                        SharedHandle<Composition::AnimationCurve> curve);
    // Widget-View-Paint-Lifecycle-Plan Tier D / D4 (2026-06-03):
    // `advanceAnimations()` deleted. Phase 4.4 already proved no
    // caller (`UIView.Update.cpp:114` comment confirms);  the
    // previously kept-as-no-op stub goes with this sweep.
    Core::Optional<float> animatedValue(const UIElementTag & tag,int key) const;
    // Widget-View-Paint-Lifecycle-Plan Tier D / D4 (2026-06-03):
    // path-node reader counterpart. `(NodeId, PathNodeX/Y,
    // subIndex=nodeIndex)` lookup against the scheduler side-table.
    // Returns nullopt for an untagged element, unknown nodeIndex, or
    // a cell with no live tween — Paint then falls through to the
    // resolved-style fallback.
    Core::Optional<float> animatedPathNodeValue(const UIElementTag & tag,
                                                int axisKey,
                                                int nodeIndex) const;
    // Phase 4.4: lazy NodeId allocation for one of this UIView's element
    // tags. `ensure*` writes the map (used by registration paths);
    // `try*` is read-only (used by `animatedValue` so an unknown tag
    // returns nullopt instead of growing the map).
    NodeId ensureElementNodeId(const UIElementTag & tag);
    Core::Optional<NodeId> tryElementNodeId(const UIElementTag & tag) const;
    // Phase 4.8: `applyAnimatedColor` / `applyAnimatedShape` deleted
    // — orphans pre-4.8 (no in-tree caller; both functions read the
    // now-deleted `elementAnimations` / `pathNodeAnimations` maps).
    // The live path resolves animated values through
    // `animatedValue()` → `AnimationScheduler` directly during
    // `UIView::paint`.
    SharedHandle<Composition::Font> resolveFallbackTextFont();
    void convertLegacyLayoutToV2();

    // Widget-View-Paint-Lifecycle-Plan Tier D / D5 (2026-06-03):
    // unified resolved-property read. Animation scheduler is queried
    // first (live tweens override resolved-style baselines); then
    // the style table written by `resolveStyles()`; finally the
    // caller's UA default. Returns the fallback only when neither
    // table holds a value of the requested type at the cell.
    template<typename T>
    T resolved(NodeId node, PropertyKey key, T fallback,
               std::uint32_t subIndex = 0) const {
        if constexpr (UIViewInternalDetail::VariantHas_v<T, AnimatedValue>){
            if(auto * scheduler = activeAnimationScheduler()){
                if(auto v = scheduler->value<T>(node, key, subIndex)){
                    return *v;
                }
            }
        }
        if(auto v = styleTable_.get<T>(node, key, subIndex)){
            return *v;
        }
        return fallback;
    }

    /// Widget-View-Paint-Lifecycle-Plan Tier D / D5 (2026-06-03):
    /// Optional sibling of `resolved<T>` for cells that may legitimately
    /// be unset (e.g., `DropShadow`: not every element has a shadow).
    /// Same chain — scheduler → style table — but returns `{}` instead
    /// of a caller-supplied fallback when neither holds the cell.
    template<typename T>
    Core::Optional<T> resolvedOptional(NodeId node, PropertyKey key,
                                       std::uint32_t subIndex = 0) const {
        if constexpr (UIViewInternalDetail::VariantHas_v<T, AnimatedValue>){
            if(auto * scheduler = activeAnimationScheduler()){
                if(auto v = scheduler->value<T>(node, key, subIndex)){
                    return *v;
                }
            }
        }
        return styleTable_.get<T>(node, key, subIndex);
    }

private:
    // Internal: same `AppWindow::activeFrameBuilder()` lookup the
    // `UIView.Animation.cpp` helpers do. Lives on Impl so the
    // templated `resolved<T>` header below can call it without
    // re-exposing the inline lookup at every call site.
    AnimationScheduler * activeAnimationScheduler() const;
};

}

#endif
