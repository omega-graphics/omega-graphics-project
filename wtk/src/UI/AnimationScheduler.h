#ifndef OMEGAWTK_UI_ANIMATIONSCHEDULER_H
#define OMEGAWTK_UI_ANIMATIONSCHEDULER_H

#include "omegaWTK/Core/Core.h"
#include "omegaWTK/UI/StyleProperty.h"          // NodeId, PropertyKey, PropertyTableKey, StyleValue
#include "omegaWTK/UI/StyleSheet.h"             // TransitionSpec (D7.2 friend hook)
#include "omegaWTK/Composition/Animation.h"     // KeyframeTrack, AnimationHandle, TimingOptions, AnimationCurve
#include "omegaWTK/Composition/Geometry.h"      // Point2D, Rect

#include <cstdint>
#include <functional>
#include <memory>
#include <variant>

namespace OmegaWTK {

class AppWindow;

// Widget-View-Paint-Lifecycle-Plan Tier D / D7.2 (2026-06-04):
// Forward-declare `StyleResolver` so the scheduler can grant it
// `friend` access to the private `transition<T>(...)` retargeting
// hook. Per Animation-Scheduler-Plan §3.7, transitions are an
// *internal* contract between the resolver and the scheduler — app
// code authors transitions through `StyleSheet` rules, never
// through this hook.
namespace StyleSheets { class StyleResolver; }

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

// Widget-View-Paint-Lifecycle-Plan Tier D / D6.1 (2026-06-03):
// `NodeId`, `allocateNodeId()`, `PropertyKey`, `PropertyTableKey`,
// `PropertyTableKeyHash`, and the `StyleValue` variant moved to the
// public header `omegaWTK/UI/StyleProperty.h` so app-authored
// `StyleSheet`s can name properties without including private
// scheduler internals. The animation-side `AnimatedValue` variant
// remains here because it carries types specific to the animation
// runtime (Point2D / Rect / TransformationParams) that the
// resolved-style table never sees.

/// Monotonic, vsync-notional frame timestamp. Until the frame pacer
/// (Frame-Pacing-Plan) lands, FrameBuilder synthesizes this from a
/// steady_clock + a per-window frame counter.
struct FrameTime {
    std::uint64_t monotonicNs = 0;
    std::uint32_t frameIndex  = 0;
};

// Widget-View-Paint-Lifecycle-Plan Tier D / D6.1 (2026-06-03):
// `AnimatedValue`, `PropertyTableKey`, `PropertyTableKeyHash` lifted
// to `omegaWTK/UI/StyleProperty.h` so the public `StyleSheet`
// vocabulary can name them. Included transitively here through that
// public header.

class AnimationScheduler {
    // Widget-View-Paint-Lifecycle-Plan Tier D / D7.2 (2026-06-04):
    // grant the resolver access to the private templated
    // `transition<T>(...)` hook. The resolver is the *only* caller —
    // app code authors transitions through `StyleSheet` rules.
    friend class StyleSheets::StyleResolver;
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

    /// Widget-View-Paint-Lifecycle-Plan Tier D / D7.2 (2026-06-04):
    /// transition entry point invoked by `StyleSheets::StyleResolver`
    /// (via friend) when a cell changes between the previous and
    /// current Style phase AND the cell has a `TransitionSpec`
    /// recorded by D6.5. Semantics (Animation-Scheduler-Plan §3.7):
    ///
    ///   * Fresh start: if no animation is active for `(node, key)`,
    ///     begin a tween with `from=prev`, `to=curr`, the spec's
    ///     timing + curve. Seed the scheduler side table with
    ///     `from` immediately so Paint reads the pre-transition
    ///     value for this frame (Tick has already run, so the
    ///     scheduler will not write the cell until the next frame).
    ///   * Smooth retarget: if an animation IS active for
    ///     `(node, key)`, capture its current sampled value from the
    ///     side table and use that as the new `from`; start a fresh
    ///     tween to the new `to` with progress reset to 0. This is
    ///     the standard CSS "smooth re-targeting" — the user sees
    ///     no jump.
    ///
    /// Friend-only: `tweenProperty` exposes the same machinery
    /// publicly for code-driven animations, but transitions are
    /// always sheet-driven and the resolver is the sole caller.
    /// Templated on the concrete value type — the resolver dispatches
    /// over the cell's variant alternative before invoking.
    template<typename T>
    Composition::AnimationHandle transition(
            NodeId node, PropertyKey key,
            T from, T to,
            const StyleSheets::TransitionSpec & spec);

    /// Widget-View-Paint-Lifecycle-Plan Tier D / D7.2 (2026-06-04):
    /// Style-phase direct seed of the side table. The public
    /// `setTableValue` asserts Phase==Tick (because Tick is the only
    /// legal writer in steady state); `transition()` needs to write
    /// the prev-value seed during Phase 2 (Style) so this frame's
    /// Paint reads the pre-transition value rather than the post-
    /// transition target sitting in `styleTable_`. Bypasses the
    /// Tick assertion; assertion contract documented at the call
    /// site.
    void seedTableFromStyle(const PropertyTableKey & key, AnimatedValue value);

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

template<typename T>
Composition::AnimationHandle AnimationScheduler::transition(
        NodeId node, PropertyKey key,
        T from, T to,
        const StyleSheets::TransitionSpec & spec){
    const PropertyTableKey tableKey{node, key, 0};

    // Smooth retarget: if an animation is already live for this
    // (node, key), capture its current sampled value from the side
    // table and use it as the new `from`. The caller's `from`
    // (previous resolved style value) is then dropped — what the
    // user saw last frame is the start point, not what the
    // stylesheet said last frame.
    if(const AnimatedValue * sampled = lookup(tableKey)){
        if(const T * sampledT = std::get_if<T>(sampled)){
            from = *sampledT;
        }
    }

    // Seed `from` into the side table so this frame's Paint reads
    // the pre-transition value. Phase order is Tick → Style → ... ;
    // transition() runs during Style, AFTER Tick. Without this seed,
    // Paint would fall through to `styleTable_` (which already holds
    // the post-transition target) and the very first frame of the
    // transition would jump to the end. Uses the friend-only
    // `seedTableFromStyle` to bypass the Tick-only assertion on
    // `setTableValue`.
    seedTableFromStyle(tableKey, AnimatedValue{from});

    // tweenProperty cancels any existing animation for this key and
    // installs a fresh one with progress=0 — exactly the retarget
    // semantic. The captured `from` (current sampled value, when a
    // prior animation was live) makes the visual transition continue
    // smoothly from where it was.
    return tweenProperty<T>(node, key, std::move(from), std::move(to),
                            spec.timing, spec.curve);
}

} // namespace OmegaWTK

#endif
