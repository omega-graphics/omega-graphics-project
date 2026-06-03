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
};

} // namespace StyleSheets
} // namespace OmegaWTK

#endif // OMEGAWTK_UI_STYLERESOLVER_H
