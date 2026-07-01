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
    __block ThemeDesc desc{};

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

    // Dynamic system NSColors resolve their concrete RGBA against
    // NSAppearance.currentDrawingAppearance, which is only set during a
    // view's draw loop. queryCurrentTheme() runs outside one — at
    // construction and, critically, inside the effectiveAppearance KVO
    // callback — where currentDrawingAppearance can still be the PREVIOUS
    // appearance, so the colors would lag the just-changed appearance by
    // one flip. Pin the resolution to the effective appearance so the
    // colors match the appearance bit computed above on the very first
    // change. `performAsCurrentDrawingAppearance:` is macOS 11+; fall
    // back to a direct read on older systems.
    void (^resolveColors)(void) = ^{
        desc.colors.controlBackground = ns_color_to_color(NSColor.controlBackgroundColor);
        desc.colors.controlForeground = ns_color_to_color(NSColor.controlTextColor);
        desc.colors.background = ns_color_to_color(NSColor.windowBackgroundColor);
        desc.colors.foreground = ns_color_to_color(NSColor.labelColor);
        desc.colors.separator = ns_color_to_color(NSColor.separatorColor);
        desc.colors.selection = ns_color_to_color(NSColor.selectedContentBackgroundColor);
        desc.colors.accent = ns_color_to_color(NSColor.controlAccentColor);
    };
    if (appearance != nil &&
        [appearance respondsToSelector:@selector(performAsCurrentDrawingAppearance:)]) {
        [appearance performAsCurrentDrawingAppearance:resolveColors];
    } else {
        resolveColors();
    }

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
