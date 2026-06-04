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
};

} // namespace StyleSheets
} // namespace OmegaWTK

#endif // OMEGAWTK_UI_STYLERESOLVER_H
