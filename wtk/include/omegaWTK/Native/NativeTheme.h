#include "omegaWTK/Core/Core.h"
#include "omegaWTK/Composition/Brush.h"

#ifndef OMEGAWTK_NATIVE_NATIVETHEME_H
#define OMEGAWTK_NATIVE_NATIVETHEME_H

namespace OmegaWTK::Native {

enum class OMEGAWTK_EXPORT ThemeAppearance : int {
    Light,
    Dark
};

struct OMEGAWTK_EXPORT ThemeDesc {
    ThemeAppearance appearance = ThemeAppearance::Light;

    struct Colors {
        Composition::Color accent { 0.f, 0.f, 0.f, 1.f };
        Composition::Color background { 1.f, 1.f, 1.f, 1.f };
        Composition::Color foreground { 0.f, 0.f, 0.f, 1.f };
        Composition::Color controlBackground { 0.9f, 0.9f, 0.9f, 1.f };
        Composition::Color controlForeground { 0.f, 0.f, 0.f, 1.f };
        Composition::Color separator { 0.8f, 0.8f, 0.8f, 1.f };
        Composition::Color selection { 0.2f, 0.4f, 0.8f, 1.f };
    } colors;

    struct Typography {
        OmegaCommon::String defaultFamily = "System";
        float defaultSize = 13.f;
        float headingSize = 17.f;
        float captionSize = 11.f;
    } typography;
};

class OMEGAWTK_EXPORT NativeThemeObserver {
public:
    INTERFACE_METHOD void onThemeSet(ThemeDesc & desc) ABSTRACT;
    virtual ~NativeThemeObserver() = default;
};

OMEGAWTK_EXPORT ThemeDesc queryCurrentTheme();

}

#endif
