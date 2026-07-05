#ifndef OMEGAWTK_UI_THEME_H
#define OMEGAWTK_UI_THEME_H

#include "omegaWTK/Core/Core.h"
#include "omegaWTK/Composition/Brush.h"     // Composition::Color
#include "omegaWTK/Native/NativeTheme.h"    // Native::ThemeAppearance
#include "omegaWTK/UI/ThemeVars.h"

namespace OmegaWTK {

/// Native-Theme-Application-Plan Tier 3 (2026-07-01): one appearance
/// variant of a custom `Theme`.
///
/// `surface` is the window background / clear color for this variant —
/// it overrides `NativeTheme.colors.background` (priority-chain row 2)
/// when the owning theme is active. `vars` are the `ThemeVars` bindings
/// that widget `StyleSheet`s resolve `Var{name}` against; the active
/// variant's `vars` is what `AppInst::themeVars()` returns while a theme
/// is installed, so switching variants (OS light/dark flip, or a forced
/// appearance) re-points every `Var` lookup with no resolver change.
struct ThemeVariant {
    Composition::Color surface { 1.f, 1.f, 1.f, 1.f };
    SharedHandle<ThemeVars> vars = nullptr;
};

class Theme;
OMEGACOMMON_SHARED_CLASS(Theme);

/// An application-registered custom theme: a named pair of Light / Dark
/// variants. Installed via `AppInst::setTheme(...)`. While installed it
/// overrides the OS surface color, but the *active variant* is still
/// chosen from the OS appearance bit (or `AppInst::forcedAppearance()`),
/// so a custom theme tracks OS dark mode unless the app opts out.
///
/// Header-only value type (Open Q2 "variant-selector" shape): the theme
/// owns two `ThemeVariant`s and picks between them by appearance.
class OMEGAWTK_EXPORT Theme {
public:
    Theme(OmegaCommon::String name, ThemeVariant light, ThemeVariant dark)
        : name_(std::move(name)),
          light_(std::move(light)),
          dark_(std::move(dark)) {}

    /// Convenience factory mirroring `ThemeVars::Create` / `Style::Create`.
    static SharedHandle<Theme> Create(OmegaCommon::String name,
                                      ThemeVariant light,
                                      ThemeVariant dark){
        return std::make_shared<Theme>(std::move(name),
                                       std::move(light), std::move(dark));
    }

    const OmegaCommon::String & name() const { return name_; }

    const ThemeVariant & light() const { return light_; }
    const ThemeVariant & dark() const { return dark_; }

    /// The variant matching `appearance` (Dark → dark, otherwise light).
    const ThemeVariant & variant(Native::ThemeAppearance appearance) const {
        return appearance == Native::ThemeAppearance::Dark ? dark_ : light_;
    }

private:
    OmegaCommon::String name_;
    ThemeVariant light_;
    ThemeVariant dark_;
};

} // namespace OmegaWTK

#endif // OMEGAWTK_UI_THEME_H
