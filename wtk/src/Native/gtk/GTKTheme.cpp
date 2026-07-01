#include "omegaWTK/Native/NativeTheme.h"
#include "omegaWTK/Composition/Brush.h"

#include <algorithm>
#include <cctype>
#include <gtk/gtk.h>
#include <string>

// X11's <X.h> (pulled in transitively via GTK) defines macros like
// `None`, `True`, `False`, `Window`, `Display` that collide with C++
// identifiers in OmegaWTK headers. NativeTheme.h is pulled BEFORE
// <gtk/gtk.h> above so its enum members survive; nothing in this TU
// needs X11 names directly.

namespace OmegaWTK::Native {

namespace {

// GTK 4 removed the synthetic GtkStyleContext construction path
// (GtkWidgetPath / gtk_style_context_new + set_path) and gdk_screen_get_default.
// Native-Theme-Application-Plan Tier 1 (2026-06-30) restores best-effort
// color extraction anyway: a *real* widget's style context still resolves
// the active theme's named colors against the display's CSS provider, so
// `populateColors` spins up a throwaway GtkWindow purely as a style-context
// host and reads the standard Adwaita named colors off it. The APIs used
// (`gtk_widget_get_style_context`, `gtk_style_context_lookup_color`) are
// deprecated in GTK 4.10 but still present — the calls are wrapped in
// deprecation-ignore guards. Every lookup is optional; a miss leaves the
// appearance-derived defaults from `applyAppearanceDefaults` in place.
// A proper CSS-parse pass replaces this heuristic later (plan §5.3.1).

/// `gtk-application-prefer-dark-theme` is the canonical GTK signal for
/// dark mode. Many distributions also encode it via the theme name's
/// `-dark` suffix (e.g. `Adwaita-dark`), so we fall back to that when
/// the explicit setting is off — keeps detection accurate on GNOME 42+
/// where the dark switch routes through `color-scheme` GSetting.
static ThemeAppearance queryAppearance(){
    GtkSettings *settings = gtk_settings_get_default();
    if(settings == nullptr){
        return ThemeAppearance::Light;
    }
    gboolean prefersDark = FALSE;
    g_object_get(settings, "gtk-application-prefer-dark-theme", &prefersDark, NULL);
    if(prefersDark){
        return ThemeAppearance::Dark;
    }
    gchar *themeName = nullptr;
    g_object_get(settings, "gtk-theme-name", &themeName, NULL);
    bool isDark = false;
    if(themeName != nullptr){
        std::string name(themeName);
        g_free(themeName);
        std::transform(name.begin(), name.end(), name.begin(),
            [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        if(name.find("dark") != std::string::npos){
            isDark = true;
        }
    }
    return isDark ? ThemeAppearance::Dark : ThemeAppearance::Light;
}

/// GTK exposes the system UI font as a PangoFontDescription string via
/// the `gtk-font-name` setting (e.g. "Cantarell 11", "Sans Bold 10").
/// Parse it for the family + nominal point size; derive heading /
/// caption sizes from the same `17/13` and `11/13` ratios the macOS /
/// Win32 backends use so widgets keep proportions across platforms.
static void populateTypography(ThemeDesc &desc){
    GtkSettings *settings = gtk_settings_get_default();
    if(settings == nullptr){
        return;
    }
    gchar *fontName = nullptr;
    g_object_get(settings, "gtk-font-name", &fontName, NULL);
    if(fontName == nullptr){
        return;
    }
    PangoFontDescription *pfd = pango_font_description_from_string(fontName);
    g_free(fontName);
    if(pfd == nullptr){
        return;
    }
    const char *family = pango_font_description_get_family(pfd);
    if(family != nullptr){
        desc.typography.defaultFamily = OmegaCommon::String(family);
    }
    int pangoSize = pango_font_description_get_size(pfd);
    if(pangoSize > 0){
        // PANGO_SCALE = 1024; pango_font_description_get_size returns
        // points * PANGO_SCALE for "absolute=false" descriptions (the
        // default for theme-derived sizes). For absolute-size
        // descriptions the value is device units; the heuristic below
        // keeps both cases sensible (point sizes never exceed ~40,
        // absolute sizes always do once scaled).
        float pt = static_cast<float>(pangoSize) / static_cast<float>(PANGO_SCALE);
        if(pango_font_description_get_size_is_absolute(pfd)){
            // Absolute sizes come back in device units — approximate
            // back to points at 72 dpi. This is wrong by the user's
            // current DPI scale, but better than leaving the field at
            // 13.f for users who configured an absolute font size.
            pt = pt * (72.f / 96.f);
        }
        if(pt > 0.f){
            desc.typography.defaultSize = pt;
            desc.typography.headingSize = pt * (17.f / 13.f);
            desc.typography.captionSize = pt * (11.f / 13.f);
        }
    }
    pango_font_description_free(pfd);
}

/// Seed a coherent palette from the appearance bit BEFORE the best-effort
/// color lookup runs. The ThemeDesc in-class initializers are a light
/// palette (white background, black text); on a dark desktop that would
/// leave a white surface if the named-color lookup misses. Mirror the
/// Win32 backend's dark values so a lookup failure still yields a
/// sensible dark surface, matching cross-platform expectations.
static void applyAppearanceDefaults(ThemeDesc &desc){
    if(desc.appearance != ThemeAppearance::Dark){
        return; // in-class light defaults already stand.
    }
    desc.colors.background = Composition::Color::create8Bit(0x1E, 0x1E, 0x1E, 255);
    desc.colors.foreground = Composition::Color::create8Bit(0xFF, 0xFF, 0xFF, 255);
    desc.colors.controlBackground = Composition::Color::create8Bit(0x2D, 0x2D, 0x2D, 255);
    desc.colors.controlForeground = Composition::Color::create8Bit(0xFF, 0xFF, 0xFF, 255);
    desc.colors.separator = Composition::Color::create8Bit(0x40, 0x40, 0x40, 255);
    desc.colors.selection = Composition::Color::create8Bit(0x00, 0x56, 0xC0, 255);
    desc.colors.accent = Composition::Color::create8Bit(0x00, 0x78, 0xD4, 255);
}

/// Resolve one named theme color off a style context into `out`. Returns
/// false (leaving `out` untouched) when the name is undefined for the
/// active theme. `gtk_style_context_lookup_color` is deprecated in GTK
/// 4.10 but still functional; guarded so it does not trip -Werror builds.
static bool lookupNamedColor(GtkStyleContext *ctx, const char *name, Composition::Color &out){
    if(ctx == nullptr){
        return false;
    }
    GdkRGBA rgba{};
    gboolean found = FALSE;
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    found = gtk_style_context_lookup_color(ctx, name, &rgba);
    G_GNUC_END_IGNORE_DEPRECATIONS
    if(!found){
        return false;
    }
    out = Composition::Color{
        static_cast<float>(rgba.red),
        static_cast<float>(rgba.green),
        static_cast<float>(rgba.blue),
        static_cast<float>(rgba.alpha)
    };
    return true;
}

/// Best-effort GTK 4 color extraction (see the file-top note). Reads the
/// standard Adwaita named colors off a throwaway GtkWindow's style
/// context. Any miss leaves the appearance-derived default in place.
static void populateColors(ThemeDesc &desc){
    GtkWidget *probe = gtk_window_new();
    if(probe == nullptr){
        return;
    }
    GtkStyleContext *ctx = nullptr;
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    ctx = gtk_widget_get_style_context(probe);
    G_GNUC_END_IGNORE_DEPRECATIONS

    lookupNamedColor(ctx, "theme_bg_color", desc.colors.background);
    lookupNamedColor(ctx, "theme_fg_color", desc.colors.foreground);
    // `theme_base_color` / `theme_text_color` are the entry/list content
    // surface — the closest classic-Adwaita analog to control colors.
    lookupNamedColor(ctx, "theme_base_color", desc.colors.controlBackground);
    lookupNamedColor(ctx, "theme_text_color", desc.colors.controlForeground);
    lookupNamedColor(ctx, "borders", desc.colors.separator);
    // Accent: libadwaita apps expose `accent_bg_color`; classic GTK
    // themes expose `theme_selected_bg_color`. Try the modern name
    // first. Selection tracks the same color.
    if(lookupNamedColor(ctx, "accent_bg_color", desc.colors.accent) ||
       lookupNamedColor(ctx, "theme_selected_bg_color", desc.colors.accent)){
        desc.colors.selection = desc.colors.accent;
    }

    gtk_window_destroy(GTK_WINDOW(probe));
}

}

ThemeDesc queryCurrentTheme() {
    ThemeDesc desc{};

    // No display → return the defaults from ThemeDesc's in-class
    // initializers. Keeps headless invocations (unit tests, build-time
    // tooling) from crashing on the GTK calls below.
    if(gdk_display_get_default() == nullptr){
        return desc;
    }

    desc.appearance = queryAppearance();

    // Seed appearance-appropriate defaults, then override per-field with
    // whatever the live theme's named colors resolve to (best-effort).
    applyAppearanceDefaults(desc);
    populateColors(desc);

    populateTypography(desc);

    return desc;
}

}
