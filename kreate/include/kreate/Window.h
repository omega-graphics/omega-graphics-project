#ifndef KREATE_WINDOW_H
#define KREATE_WINDOW_H

#include <memory>
#include "Base.h"

namespace OmegaGTE {
    struct NativeRenderTargetDescriptor;
}

namespace Kreate {

struct KREATE_EXPORT WindowDesc {
    const char *title;
    unsigned width;
    unsigned height;
};

class KREATE_EXPORT Window {
public:
    static std::unique_ptr<Window> create(const WindowDesc &desc);

    [[nodiscard]] bool shouldClose() const;
    void pollEvents();

    [[nodiscard]] unsigned width() const;
    [[nodiscard]] unsigned height() const;

    /// Fills a NativeRenderTargetDescriptor with this window's platform surface.
    /// Implemented per-platform so ObjC++/Win32/X11 details stay out of App.cpp.
    void fillNativeRenderTargetDesc(OmegaGTE::NativeRenderTargetDescriptor &desc) const;

    ~Window();

private:
    Window();
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Kreate

#endif
