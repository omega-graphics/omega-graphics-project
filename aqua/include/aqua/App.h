#ifndef AQUA_APP_H
#define AQUA_APP_H

#include "Window.h"
#include <OmegaGTE.h>

namespace Aqua {

struct AQUA_EXPORT AppDesc {
    WindowDesc window;
};

class AQUA_EXPORT App {
public:
    explicit App(const AppDesc &desc);
    virtual ~App();

    Window &window();
    OmegaGTE::GTE &gte();
    SharedHandle<OmegaGTE::GENativeRenderTarget> &renderTarget();

    /// Called once after GTE and the render target are ready.
    virtual void onInit() {}

    /// Called every frame. Override this in your game.
    virtual void onFrame() {}

    /// Main loop — called by Main.cpp. Polls events and calls onFrame().
    void run();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

/// Each game implements this factory. Main.cpp calls it to get the App instance.
AQUA_EXPORT std::unique_ptr<App> CreateApp();

} // namespace Aqua

#endif
