#include "omegaWTK/Native/NativeTheme.h"
#include "omegaWTK/Composition/Brush.h"
#import <AppKit/AppKit.h>

namespace OmegaWTK::Native {

static Composition::Color ns_color_to_color(NSColor *nsColor) {
    if (nsColor == nil) {
        return Composition::Color{0.f, 0.f, 0.f, 1.f};
    }
    NSColor *rgb = [nsColor colorUsingColorSpace:NSColorSpace.genericRGBColorSpace];
    if (rgb == nil) {
        return Composition::Color{0.f, 0.f, 0.f, 1.f};
    }
    CGFloat r = 0, g = 0, b = 0, a = 0;
    [rgb getRed:&r green:&g blue:&b alpha:&a];
    return Composition::Color{
        static_cast<float>(r),
        static_cast<float>(g),
        static_cast<float>(b),
        static_cast<float>(a)
    };
}

ThemeDesc queryCurrentTheme() {
    ThemeDesc desc{};

    NSAppearance *appearance = [NSApp effectiveAppearance];
    if (appearance != nil) {
        NSAppearanceName best = [appearance bestMatchFromAppearancesWithNames:@[
            NSAppearanceNameAqua,
            NSAppearanceNameDarkAqua
        ]];
        if ([best isEqual:NSAppearanceNameDarkAqua]) {
            desc.appearance = ThemeAppearance::Dark;
        } else {
            desc.appearance = ThemeAppearance::Light;
        }
    }

    NSColor *controlBg = NSColor.controlBackgroundColor;
    NSColor *controlFg = NSColor.controlTextColor;
    NSColor *windowBg = NSColor.windowBackgroundColor;
    NSColor *label = NSColor.labelColor;
    NSColor *separator = NSColor.separatorColor;
    NSColor *selection = NSColor.selectedContentBackgroundColor;
    NSColor *accent = NSColor.controlAccentColor;
    desc.colors.controlBackground = ns_color_to_color(controlBg);
    desc.colors.controlForeground = ns_color_to_color(controlFg);
    desc.colors.background = ns_color_to_color(windowBg);
    desc.colors.foreground = ns_color_to_color(label);
    desc.colors.separator = ns_color_to_color(separator);
    desc.colors.selection = ns_color_to_color(selection);
    desc.colors.accent = ns_color_to_color(accent);

    NSFont *systemFont = [NSFont systemFontOfSize:13];
    if (systemFont != nil && systemFont.displayName != nil) {
        desc.typography.defaultFamily = [systemFont.displayName UTF8String];
    }
    desc.typography.defaultSize = 13.f;
    desc.typography.headingSize = 17.f;
    desc.typography.captionSize = 11.f;

    return desc;
}

}
