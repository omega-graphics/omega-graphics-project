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

// GTK 4 removed GtkWidgetPath / synthetic GtkStyleContext color extraction
// (and gdk_screen_get_default). Per the §2.15 scope decision the GTK 4 theme
// query keeps only the dark/light appearance + typography; theme color
// extraction is GTK-3-only and the ThemeDesc color defaults stand under GTK 4.

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


    populateTypography(desc);

    return desc;
}

}
