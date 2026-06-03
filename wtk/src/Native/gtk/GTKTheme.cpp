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

static Composition::Color gdkRgbaToColor(const GdkRGBA &rgba){
    return Composition::Color{
        static_cast<float>(rgba.red),
        static_cast<float>(rgba.green),
        static_cast<float>(rgba.blue),
        static_cast<float>(rgba.alpha)
    };
}

/// `gtk_style_context_lookup_color` resolves a *named* CSS color from
/// the current theme (`theme_bg_color`, `theme_fg_color`,
/// `theme_selected_bg_color`, `borders`, etc.) into a concrete RGBA.
/// Returns true and writes `out` if the lookup succeeds; otherwise
/// leaves `out` untouched so the caller's default survives.
static bool lookupNamedColor(GtkStyleContext *ctx,const char *name,Composition::Color &out){
    if(ctx == nullptr || name == nullptr){
        return false;
    }
    GdkRGBA rgba{};
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    // Deprecated in GTK 3.8 but still functional in GTK 3.x and the
    // only API that resolves named CSS theme colors without rendering.
    gboolean found = gtk_style_context_lookup_color(ctx, name, &rgba);
G_GNUC_END_IGNORE_DEPRECATIONS
    if(!found){
        return false;
    }
    out = gdkRgbaToColor(rgba);
    return true;
}

/// Build a synthetic GtkStyleContext for the given widget-type path.
/// `leafType` may be 0 to query at the toplevel window level only.
/// Returns nullptr if no default GDK screen is available (headless).
/// Ownership transferred to caller — release with g_object_unref.
static GtkStyleContext *makeContextForPath(GType rootType,GType leafType,const char *leafClass){
    if(gdk_screen_get_default() == nullptr){
        return nullptr;
    }
    GtkWidgetPath *path = gtk_widget_path_new();
    gtk_widget_path_append_type(path, rootType);
    if(leafType != 0){
        gtk_widget_path_append_type(path, leafType);
    }
    GtkStyleContext *ctx = gtk_style_context_new();
    gtk_style_context_set_path(ctx, path);
    if(leafClass != nullptr){
        gtk_style_context_add_class(ctx, leafClass);
    }
    gtk_widget_path_unref(path);
    return ctx;
}

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

    GtkStyleContext *windowCtx = makeContextForPath(GTK_TYPE_WINDOW, 0, nullptr);
    if(windowCtx != nullptr){
        lookupNamedColor(windowCtx, "theme_bg_color",          desc.colors.background);
        lookupNamedColor(windowCtx, "theme_fg_color",          desc.colors.foreground);
        // `theme_selected_bg_color` doubles as the OS accent on every
        // mainstream GTK theme. macOS routes accent through
        // controlAccentColor; Win32 hardcodes a Windows blue. Mirroring
        // GTK's selection color here keeps custom widgets' "accent"
        // hooks visually consistent with the rest of the desktop.
        Composition::Color selectionColor = desc.colors.selection;
        if(lookupNamedColor(windowCtx, "theme_selected_bg_color", selectionColor)){
            desc.colors.selection = selectionColor;
            desc.colors.accent = selectionColor;
        }
        // `borders` is the canonical GTK name for the widget border /
        // separator color. Some minimal themes don't ship it; fall
        // through to the default in that case.
        lookupNamedColor(windowCtx, "borders", desc.colors.separator);
        g_object_unref(windowCtx);
    }

    GtkStyleContext *buttonCtx = makeContextForPath(
        GTK_TYPE_WINDOW, GTK_TYPE_BUTTON, GTK_STYLE_CLASS_BUTTON);
    if(buttonCtx != nullptr){
        GtkStateFlags state = gtk_style_context_get_state(buttonCtx);
        GdkRGBA fg{};
        gtk_style_context_get_color(buttonCtx, state, &fg);
        desc.colors.controlForeground = gdkRgbaToColor(fg);

G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        // `gtk_style_context_get_background_color` is the only API in
        // GTK 3.x that resolves the rendered button background to a
        // concrete RGBA without rasterizing into a Cairo surface. Its
        // deprecation in GTK 3.16 was driven by a desire to discourage
        // theme-color extraction from non-paint code paths — exactly
        // what we're doing on purpose for the theme query. The
        // replacement (`gtk_render_background` against a temporary
        // surface and sample) would round-trip through Cairo for the
        // same end result.
        GdkRGBA bg{};
        gtk_style_context_get_background_color(buttonCtx, state, &bg);
G_GNUC_END_IGNORE_DEPRECATIONS
        // Some themes return a fully transparent background here
        // (relying on the parent surface to show through); fall back
        // to the window background so controls don't render with no
        // contrast against `background`.
        if(bg.alpha > 0.0){
            desc.colors.controlBackground = gdkRgbaToColor(bg);
        }
        else {
            desc.colors.controlBackground = desc.colors.background;
        }

        g_object_unref(buttonCtx);
    }

    populateTypography(desc);

    return desc;
}

}
