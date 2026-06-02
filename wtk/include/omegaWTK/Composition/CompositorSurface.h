#include "omegaWTK/Core/Core.h"
#include "CompositeFrame.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>

#ifndef OMEGAWTK_COMPOSITION_COMPOSITORSURFACE_H
#define OMEGAWTK_COMPOSITION_COMPOSITORSURFACE_H

namespace OmegaWTK {
    // Forward-decl only — CompositorSurface stores a back-pointer to its
    // owning AppWindow so Compositor::drainWindowSurfaces can consult
    // AppWindow::isNativeReady() without consuming the surface's pending
    // frame. See `wtk/.plans/NativeWindow-Ready-Signal-Plan.md` §3.5(A).
    class AppWindow;
}

namespace OmegaWTK::Composition {

class CompositorSurface {
    std::mutex mutex_;
    SharedHandle<CompositeFrame> latestFrame_;
    std::atomic<uint64_t> generation_ {0};
    uint64_t consumedGeneration_ = 0;
    std::function<void()> onDeposit_;

    /// NativeWindow-Ready-Signal-Plan §3.5(A): back-edge from the
    /// surface to its owning AppWindow. Atomic + raw pointer because
    /// (a) it's set once at AppWindow::setRootWidget time and never
    /// mutated again, (b) AppWindow outlives its CompositorSurface by
    /// construction, and (c) drainWindowSurfaces reads it without
    /// holding the surface lock.
    std::atomic<::OmegaWTK::AppWindow *> ownerAppWindow_ {nullptr};

public:
    void deposit(SharedHandle<CompositeFrame> frame);

    SharedHandle<CompositeFrame> consume();

    bool hasPendingUpdate() const;

    uint64_t generation() const;

    /// Set a callback fired (outside the surface lock) every time a new
    /// frame is deposited. The compositor uses this to wake its frame
    /// loop without polling.
    void setOnDeposit(std::function<void()> callback);

    /// Plan step 3: set / query the back-pointer to the AppWindow
    /// whose NativeWindow gates this surface's render dispatch.
    /// AppWindow::setRootWidget calls setOwnerAppWindow(this) before
    /// registering the surface with the Compositor so any synchronous
    /// deposit from registration finds a non-null owner.
    void setOwnerAppWindow(::OmegaWTK::AppWindow * appWindow);
    ::OmegaWTK::AppWindow * ownerAppWindow() const;
};

}

#endif
