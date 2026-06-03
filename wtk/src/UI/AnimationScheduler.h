#ifndef OMEGAWTK_UI_ANIMATIONSCHEDULER_H
#define OMEGAWTK_UI_ANIMATIONSCHEDULER_H

#include "omegaWTK/Core/Core.h"
#include "omegaWTK/Composition/Animation.h"   // KeyframeTrack, AnimationHandle, TimingOptions, AnimationCurve
#include "omegaWTK/Composition/Brush.h"        // Color, Brush
#include "omegaWTK/Composition/Geometry.h"     // Point2D, Rect
#include "omegaWTK/Composition/Layer.h"        // LayerEffect::{DropShadowParams,TransformationParams}

#include <cstdint>
#include <functional>
#include <memory>
#include <variant>

namespace OmegaWTK {

class AppWindow;

// UIView-Render-Redesign Tier 4 Block 2, Phase 4.3 — folds
// Animation-Scheduler-Plan Tier A.
//
// Per-window animation runtime. Owns the active-animation set and the
// (NodeId, PropertyKey) side table the Paint phase will read from. One
// per AppWindow, constructed alongside the FrameBuilder; ticked once per
// frame from FrameBuilder Phase 1 (Tick).
//
// Phase 4.3 is ADDITIVE: this lands alongside the live ViewAnimator /
// LayerAnimator path, which still drives every real animation. Nothing
// reads the side table yet (UIView routes onto the scheduler in 4.4), so
// in practice the active set is empty and tick() is a no-op-over-empty
// each frame. The validator is "tick fires once per frame."
//
// Placement is UI-private (like FrameBuilder), per the developer's
// 2026-05-29 decision — overriding Animation-Scheduler-Plan §4's public
// Composition placement. The app-facing surface is exposed later via an
// AppWindow accessor; for now the only caller is FrameBuilder.

/// A scene node (`View`) identity. The NodeId -> View mapping does not
/// exist yet (4.7); in 4.3 the scheduler only stores values against it.
using NodeId = std::uint64_t;

/// Phase 4.4: process-wide atomic counter behind `View::nodeId()` and the
/// per-`(UIView, UIElementTag)` NodeIds UIView allocates lazily. Stable
/// across the lifetime of the process; never reused. The counter lives
/// here (alongside the only consumer of NodeIds) rather than on `View`
/// itself so element identities — which are not `View`s — can share the
/// allocator without leaking through the public View surface.
NodeId allocateNodeId();

/// Animatable property channels (Animation-Scheduler-Plan §3.2). The
/// Layout* keys are layout-affecting (see isLayoutProperty); the rest are
/// paint-only.
enum class PropertyKey : std::uint16_t {
    // Visuals (read by Paint)
    BackgroundColor,
    BorderColor,
    BorderWidth,
    Opacity,
    FillBrush,
    ShadowOffsetX, ShadowOffsetY, ShadowBlur, ShadowColor,
    TransformX, TransformY,
    TransformScaleX, TransformScaleY,
    TransformRotation,
    // Widget-View-Paint-Lifecycle-Plan Tier D / D5 (2026-06-03):
    // aggregate drop-shadow cell. Animatable shadow channels still
    // ride their own (ShadowOffsetX/Y, ShadowBlur, ShadowColor)
    // scalar keys — `DropShadow` carries the resolved-style baseline
    // the Shadow channel animations layer on top of during Paint.
    DropShadow,

    // Text
    TextColor, TextSize,
    // Widget-View-Paint-Lifecycle-Plan Tier D / D5 (2026-06-03):
    // text-style cells written by `resolveStyles()` and read during
    // Paint via the `resolved<T>()` helper. `TextFont` holds a
    // `SharedHandle<Composition::Font>`, `TextLayout` a
    // `Composition::TextLayoutDescriptor`, `TextLineLimit` a uint32.
    TextFont, TextLayout, TextLineLimit,

    // Layout (read by the Layout phase — layout-affecting)
    LayoutWidth, LayoutHeight,
    LayoutX, LayoutY,

    // Sub-indexed (subIndex addresses the path node / gradient stop)
    PathNodeX, PathNodeY,

    // App-allocated keys start here
    UserDefined = 0x8000
};

/// Monotonic, vsync-notional frame timestamp. Until the frame pacer
/// (Frame-Pacing-Plan) lands, FrameBuilder synthesizes this from a
/// steady_clock + a per-window frame counter.
struct FrameTime {
    std::uint64_t monotonicNs = 0;
    std::uint32_t frameIndex  = 0;
};

/// One side-table cell. `BrushPtr` in the plan is a
/// `Core::SharedPtr<Composition::Brush>` in this tree. `std::monostate`
/// is the empty/unset state.
using AnimatedValue = std::variant<
    std::monostate,
    float, int, std::uint32_t,
    Composition::Color,
    Composition::Point2D,
    Composition::Rect,
    Core::SharedPtr<Composition::Brush>,
    Composition::LayerEffect::DropShadowParams,
    Composition::LayerEffect::TransformationParams>;

/// Side-table / active-property key. `subIndex` is 0 for the common case
/// and addresses path nodes / gradient stops for the *At variants.
struct PropertyTableKey {
    NodeId        node     = 0;
    PropertyKey   key      = PropertyKey::Opacity;
    std::uint32_t subIndex = 0;

    bool operator==(const PropertyTableKey & other) const {
        return node == other.node && key == other.key && subIndex == other.subIndex;
    }
};

struct PropertyTableKeyHash {
    std::size_t operator()(const PropertyTableKey & k) const {
        // node dominates the entropy; fold key+subIndex into the low bits.
        std::size_t h = std::hash<std::uint64_t>{}(k.node);
        h ^= (static_cast<std::size_t>(k.key) + 0x9e3779b97f4a7c15ULL +
              (h << 6) + (h >> 2));
        h ^= (static_cast<std::size_t>(k.subIndex) + 0x9e3779b97f4a7c15ULL +
              (h << 6) + (h >> 2));
        return h;
    }
};

class AnimationScheduler {
public:
    /// Per-window animation counters (Animation-Scheduler-Plan §3.10).
    /// Refreshed each tick().
    struct Stats {
        std::uint32_t activeProperty = 0;
        std::uint32_t activeCallback = 0;
        std::uint32_t ticksThisFrame = 0;   // animations advanced this tick
        std::uint32_t appliesFired   = 0;   // callback apply()s fired this tick
        std::uint64_t tickElapsedNs  = 0;   // wall time spent in the last tick
    };

    explicit AnimationScheduler(AppWindow & window);
    ~AnimationScheduler();

    AnimationScheduler(const AnimationScheduler &)             = delete;
    AnimationScheduler & operator=(const AnimationScheduler &) = delete;

    // ---- Property animations (written to the side table) ------------

    template<typename T>
    Composition::AnimationHandle animateProperty(
            NodeId node, PropertyKey key,
            Composition::KeyframeTrack<T> track,
            Composition::TimingOptions timing = {});

    template<typename T>
    Composition::AnimationHandle tweenProperty(
            NodeId node, PropertyKey key,
            T from, T to,
            Composition::TimingOptions timing = {},
            SharedHandle<Composition::AnimationCurve> curve = Composition::AnimationCurve::Linear());

    template<typename T>
    Composition::AnimationHandle animatePropertyAt(
            NodeId node, PropertyKey key, std::uint32_t subIndex,
            Composition::KeyframeTrack<T> track,
            Composition::TimingOptions timing = {});

    /// Widget-View-Paint-Lifecycle-Plan Tier D / D4 (2026-06-03):
    /// from→to convenience over `animatePropertyAt`, mirroring
    /// `tweenProperty` but threading `subIndex` so path-node and
    /// gradient-stop tweens key by `(node, key, subIndex)` instead
    /// of collapsing every sub-cell onto `subIndex=0`. Used by
    /// `UIView::Impl::startOrUpdatePathNodeAnimation` to back
    /// `UIView::animatePathNode`.
    template<typename T>
    Composition::AnimationHandle tweenPropertyAt(
            NodeId node, PropertyKey key, std::uint32_t subIndex,
            T from, T to,
            Composition::TimingOptions timing = {},
            SharedHandle<Composition::AnimationCurve> curve = Composition::AnimationCurve::Linear());

    // ---- Callback animations (fire apply() from tick) ---------------

    template<typename T>
    Composition::AnimationHandle animate(
            Composition::KeyframeTrack<T> track,
            std::function<void(const T &)> apply,
            Composition::TimingOptions timing = {});

    template<typename T>
    Composition::AnimationHandle tween(
            T from, T to,
            std::function<void(const T &)> apply,
            Composition::TimingOptions timing = {},
            SharedHandle<Composition::AnimationCurve> curve = Composition::AnimationCurve::Linear());

    // ---- Side-table reads (Paint phase) -----------------------------

    template<typename T>
    Core::Optional<T> value(NodeId node, PropertyKey key,
                            std::uint32_t subIndex = 0) const;

    bool hasAnyAnimationFor(NodeId node) const;

    // ---- Lifecycle --------------------------------------------------

    /// Phase 1 (Tick). Advances every active animation against `now`,
    /// writing resolved values into the side table (property anims) and
    /// firing apply() (callback anims). Completed / cancelled animations
    /// are reaped here.
    void tick(FrameTime now);

    /// Cancel + drop every animation (and side-table cell) targeting
    /// `node`. Called when a node leaves the tree.
    void cancelAllForNode(NodeId node);

    void pauseAll();
    void resumeAll();
    void cancelAll();

    Stats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    // Type-erased registration shared by the templated entry points so
    // Impl stays non-templated (Animation-Scheduler-Plan §4). `sample(t)`
    // samples the captured track at normalized t in [0,1] and writes the
    // side table (property) or fires apply() (callback).
    Composition::AnimationHandle registerProperty(
            const PropertyTableKey & key,
            std::function<void(float)> sample,
            Composition::TimingOptions timing,
            bool layoutAffecting);
    Composition::AnimationHandle registerCallback(
            std::function<void(float)> sample,
            Composition::TimingOptions timing);

    // Non-templated side-table mutator/reader the header templates call.
    void                  setTableValue(const PropertyTableKey & key, AnimatedValue value);
    const AnimatedValue * lookup(const PropertyTableKey & key) const;

    static bool isLayoutProperty(PropertyKey key) {
        return key == PropertyKey::LayoutWidth  || key == PropertyKey::LayoutHeight ||
               key == PropertyKey::LayoutX       || key == PropertyKey::LayoutY;
    }
};

// ===== templated definitions =====

template<typename T>
Composition::AnimationHandle AnimationScheduler::animateProperty(
        NodeId node, PropertyKey key,
        Composition::KeyframeTrack<T> track,
        Composition::TimingOptions timing){
    const PropertyTableKey tableKey{node, key, 0};
    auto sample = [this, tableKey, track = std::move(track)](float t){
        setTableValue(tableKey, AnimatedValue{track.sample(t)});
    };
    return registerProperty(tableKey, std::move(sample), timing, isLayoutProperty(key));
}

template<typename T>
Composition::AnimationHandle AnimationScheduler::animatePropertyAt(
        NodeId node, PropertyKey key, std::uint32_t subIndex,
        Composition::KeyframeTrack<T> track,
        Composition::TimingOptions timing){
    const PropertyTableKey tableKey{node, key, subIndex};
    auto sample = [this, tableKey, track = std::move(track)](float t){
        setTableValue(tableKey, AnimatedValue{track.sample(t)});
    };
    return registerProperty(tableKey, std::move(sample), timing, isLayoutProperty(key));
}

template<typename T>
Composition::AnimationHandle AnimationScheduler::tweenProperty(
        NodeId node, PropertyKey key,
        T from, T to,
        Composition::TimingOptions timing,
        SharedHandle<Composition::AnimationCurve> curve){
    OmegaCommon::Vector<Composition::KeyframeValue<T>> keys;
    keys.push_back(Composition::KeyframeValue<T>{0.f, std::move(from), std::move(curve)});
    keys.push_back(Composition::KeyframeValue<T>{1.f, std::move(to),   nullptr});
    return animateProperty<T>(node, key, Composition::KeyframeTrack<T>::From(keys), timing);
}

template<typename T>
Composition::AnimationHandle AnimationScheduler::tweenPropertyAt(
        NodeId node, PropertyKey key, std::uint32_t subIndex,
        T from, T to,
        Composition::TimingOptions timing,
        SharedHandle<Composition::AnimationCurve> curve){
    OmegaCommon::Vector<Composition::KeyframeValue<T>> keys;
    keys.push_back(Composition::KeyframeValue<T>{0.f, std::move(from), std::move(curve)});
    keys.push_back(Composition::KeyframeValue<T>{1.f, std::move(to),   nullptr});
    return animatePropertyAt<T>(node, key, subIndex,
                                Composition::KeyframeTrack<T>::From(keys), timing);
}

template<typename T>
Composition::AnimationHandle AnimationScheduler::animate(
        Composition::KeyframeTrack<T> track,
        std::function<void(const T &)> apply,
        Composition::TimingOptions timing){
    auto sample = [track = std::move(track), apply = std::move(apply)](float t){
        apply(track.sample(t));
    };
    return registerCallback(std::move(sample), timing);
}

template<typename T>
Composition::AnimationHandle AnimationScheduler::tween(
        T from, T to,
        std::function<void(const T &)> apply,
        Composition::TimingOptions timing,
        SharedHandle<Composition::AnimationCurve> curve){
    OmegaCommon::Vector<Composition::KeyframeValue<T>> keys;
    keys.push_back(Composition::KeyframeValue<T>{0.f, std::move(from), std::move(curve)});
    keys.push_back(Composition::KeyframeValue<T>{1.f, std::move(to),   nullptr});
    return animate<T>(Composition::KeyframeTrack<T>::From(keys), std::move(apply), timing);
}

template<typename T>
Core::Optional<T> AnimationScheduler::value(
        NodeId node, PropertyKey key, std::uint32_t subIndex) const{
    if(const AnimatedValue * cell = lookup({node, key, subIndex})){
        if(const T * typed = std::get_if<T>(cell)){
            return *typed;
        }
    }
    return {};
}

} // namespace OmegaWTK

#endif
