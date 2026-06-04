#ifndef OMEGAWTK_UI_STYLERESOLVER_H
#define OMEGAWTK_UI_STYLERESOLVER_H

#include "omegaWTK/Core/Core.h"
#include "omegaWTK/UI/StyleSheet.h"

namespace OmegaWTK {

class UIView;

namespace StyleSheets {

/// Widget-View-Paint-Lifecycle-Plan Tier D / D6.3 (2026-06-03):
/// the cascade walker. Consumes the target view's owning
/// `AppWindow`'s style-sheet stack and writes one cell per resolved
/// property into the view's per-property style table (D5).
///
/// Integration shape (decided 2026-06-03): **layered**. The resolver
/// runs at the top of `UIView::resolveStyles()`, before the inline-
/// style writes. Inline `Style` declarations from the legacy
/// `setStyle()` aggregate still author-override sheets — they
/// overwrite cells the resolver wrote because they are written last
/// into the same `styleTable_`. A future tier may fold the inline
/// aggregate into a synthetic top-of-stack sheet; D6.3 keeps the
/// inline path exactly as D5 left it.
///
/// Matcher complexity: Tier-1 single-compound selectors only —
/// tag + (id, classes once D6.4 wires them) + pseudo-class subset.
/// Cascade comparator follows `StyleRule::beats()`. A view-level
/// rule matches against `UIView::tag()`; an element-level rule
/// matches against each element's `UIElementTag`. Property-key
/// scope (view vs element) decides which `NodeId` the cell lands on.
class OMEGAWTK_EXPORT StyleResolver {
public:
    static void apply(UIView & view);

    /// Widget-View-Paint-Lifecycle-Plan Tier D / D7.2 (2026-06-04):
    /// transition firing pass. Called by `UIView::resolveStyles()`
    /// AFTER both the sheet cascade (`apply` above) and the inline-
    /// `Style` writes have settled into `styleTable_`. Compares the
    /// just-resolved cells to the previous-frame snapshot
    /// (`previousStyleTable_`) for every `(node, key)` that has a
    /// `TransitionSpec` recorded in `sheetBindings_.transitions`,
    /// and calls `AnimationScheduler::transition<T>(node, key, prev,
    /// curr, spec)` via the `friend` hook when prev != curr.
    /// Cells without a transition record snap to the new value (the
    /// resolver / inline writes have already populated
    /// `styleTable_`; Paint reads it directly). Type dispatch
    /// happens through `std::visit` on the variant; only types in
    /// both `StyleValue` and the scheduler's `AnimatedValue`
    /// (`Color`, `uint32_t`, `Brush` handle, `DropShadowParams`)
    /// trigger a transition — non-animatable types snap silently.
    static void applyTransitions(UIView & view);

    /// Widget-View-Paint-Lifecycle-Plan Tier D / D7.3 (2026-06-04):
    /// keyframe-animation binding pass. Runs alongside / after
    /// `applyTransitions` during `UIView::resolveStyles()`. Walks
    /// `sheetBindings_.animationBindings` (one record per node,
    /// populated by the cascade in `apply()`) and reconciles against
    /// `Impl::activeKeyframeBindings_`:
    ///   * binding NEW on this node → look up the named
    ///     `KeyframeAnimation` in the AppWindow's sheet stack
    ///     (later sheets win on name collision), then call
    ///     `scheduler.animateProperty<AnimatedValue>(node, key,
    ///     track, timing)` once per property in the animation.
    ///     Track the returned handles so they can be cancelled.
    ///   * binding STILL ACTIVE with the same name → no-op
    ///     (preserves the running animation, matching CSS
    ///     `animation` semantics where the same declaration does
    ///     not restart).
    ///   * binding STILL ACTIVE but name changed → cancel the old
    ///     handles, start the new animation fresh.
    ///   * binding NO LONGER PRESENT → cancel the handles, remove
    ///     the tracking entry.
    /// Per-property animations use the scheduler's existing
    /// `animateProperty<T>` template with `T = AnimatedValue` — the
    /// `KeyframeTrack<AnimatedValue>` carries the type-erased
    /// per-property values and the D7.3
    /// `KeyframeLerp<AnimatedValue>` specialization (in
    /// `wtk/src/UI/AnimationScheduler.h`) dispatches lerp per
    /// variant alternative.
    static void applyKeyframeBindings(UIView & view);
};

} // namespace StyleSheets
} // namespace OmegaWTK

#endif // OMEGAWTK_UI_STYLERESOLVER_H
