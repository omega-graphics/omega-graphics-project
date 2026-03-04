#include "omegaWTK/Native/NativeTheme.h"
#include "omegaWTK/Composition/Brush.h"
#include <windows.h>

namespace OmegaWTK::Native {

static Composition::Color colorref_to_color(COLORREF cr) {
    return Composition::Color::create8Bit(
        GetRValue(cr),
        GetGValue(cr),
        GetBValue(cr),
        255
    );
}

static bool query_apps_use_light_theme() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     0, KEY_READ, &key) != ERROR_SUCCESS) {
        return true;
    }
    DWORD value = 1;
    DWORD size = sizeof(value);
    if (RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, nullptr,
                         reinterpret_cast<LPBYTE>(&value), &size) != ERROR_SUCCESS) {
        RegCloseKey(key);
        return true;
    }
    RegCloseKey(key);
    return value != 0;
}

ThemeDesc queryCurrentTheme() {
    ThemeDesc desc{};

    desc.appearance = query_apps_use_light_theme() ? ThemeAppearance::Light : ThemeAppearance::Dark;

    if (desc.appearance == ThemeAppearance::Dark) {
        desc.colors.background = Composition::Color::create8Bit(0x1E, 0x1E, 0x1E, 255);
        desc.colors.foreground = Composition::Color::create8Bit(0xFF, 0xFF, 0xFF, 255);
        desc.colors.controlBackground = Composition::Color::create8Bit(0x2D, 0x2D, 0x2D, 255);
        desc.colors.controlForeground = Composition::Color::create8Bit(0xFF, 0xFF, 0xFF, 255);
        desc.colors.separator = Composition::Color::create8Bit(0x40, 0x40, 0x40, 255);
        desc.colors.selection = Composition::Color::create8Bit(0x00, 0x56, 0xC0, 255);
        desc.colors.accent = Composition::Color::create8Bit(0x00, 0x78, 0xD4, 255);
    } else {
        desc.colors.background = colorref_to_color(GetSysColor(COLOR_WINDOW));
        desc.colors.foreground = colorref_to_color(GetSysColor(COLOR_WINDOWTEXT));
        desc.colors.controlBackground = colorref_to_color(GetSysColor(COLOR_3DFACE));
        desc.colors.controlForeground = colorref_to_color(GetSysColor(COLOR_BTNTEXT));
        desc.colors.separator = colorref_to_color(GetSysColor(COLOR_3DSHADOW));
        desc.colors.selection = colorref_to_color(GetSysColor(COLOR_HIGHLIGHT));
        desc.colors.accent = Composition::Color::create8Bit(0x00, 0x78, 0xD4, 255);
    }

    desc.typography.defaultFamily = "Segoe UI";
    desc.typography.defaultSize = 13.f;
    desc.typography.headingSize = 17.f;
    desc.typography.captionSize = 11.f;

    return desc;
}

}
