#ifndef OMEGAWTK_UI_THEMEVARS_H
#define OMEGAWTK_UI_THEMEVARS_H

#include "omegaWTK/Core/Core.h"
#include "omegaWTK/UI/StyleProperty.h"

namespace OmegaWTK {

class ThemeVars;
OMEGACOMMON_SHARED_CLASS(ThemeVars);

/// Widget-View-Paint-Lifecycle-Plan Tier D / D7.1 (2026-06-04):
/// the process-wide named-`StyleValue` map referenced by
/// `StyleSheets::Var` in sheet rules. `AppInst` owns the active
/// theme as a `SharedHandle<ThemeVars>`; the `StyleResolver`
/// substitutes `Var{name}` with the bound concrete value when
/// the cascade is being applied.
///
/// Shape parallels `StyleSheet`: immutable-once-built, produced
/// through a `Builder`, shareable across `AppWindow`s via the
/// `SharedHandle`. To mutate a theme, build a sibling handle and
/// hand it to `AppInst::setThemeVars(...)` — the swap dirties the
/// style cascade on every known window so cell-level Var
/// substitutions re-evaluate against the new bindings.
///
/// Resolution semantics (D7.1):
///   * A `Var` whose name is bound substitutes to the bound value.
///   * A `Var` whose name is unbound (no theme installed, or the
///     name is missing, or the bound value is itself a `Var` —
///     chains are not followed in D7.1) skips the cell write; the
///     inline-`Style` writes that follow the resolver still get
///     the chance to author the property, matching the CSS
///     `var()` fallthrough behavior.
class OMEGAWTK_EXPORT ThemeVars {
public:
    /// Empty handle. `AppInst::setThemeVars(ThemeVars::Create())`
    /// is the explicit "clear all theme bindings" call; the more
    /// common path is to build a populated handle through `Builder`.
    static SharedHandle<ThemeVars> Create();

    /// Builder produces an immutable handle. Reusing the builder
    /// after `build()` seeds a sibling theme.
    class OMEGAWTK_EXPORT Builder {
    public:
        Builder & set(const OmegaCommon::String & name, StyleValue value);
        SharedHandle<ThemeVars> build() const;
    private:
        OmegaCommon::Map<OmegaCommon::String, StyleValue> values_;
    };

    /// Returns the bound `StyleValue` if `name` is present in the
    /// theme, otherwise an empty optional. The resolver treats a
    /// `std::monostate` payload identically to "name not present".
    Core::Optional<StyleValue> lookup(const OmegaCommon::String & name) const;

    bool empty() const;
    const OmegaCommon::Map<OmegaCommon::String, StyleValue> & values() const;

private:
    ThemeVars() = default;
    friend class Builder;

    OmegaCommon::Map<OmegaCommon::String, StyleValue> values_;
};

} // namespace OmegaWTK

#endif // OMEGAWTK_UI_THEMEVARS_H
