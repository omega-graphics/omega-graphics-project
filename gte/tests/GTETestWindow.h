#ifndef OMEGAGTE_TESTS_GTETESTWINDOW_H
#define OMEGAGTE_TESTS_GTETESTWINDOW_H

#include <omegaGTE/GE.h>
#include <functional>
#include <string>

// GTETestWindow — the backend-neutral GUI surface every windowed GTE test
// shares. See gte/.plans/GTETestWindow-CrossBackend-Plan.md. The header is
// pure C++: no <windows.h>, no #import, no GTK pull-ins. The per-platform
// toolkit (Win32 / Cocoa / GTK / Wayland / Android) lives entirely behind the
// per-backend implementation files, selected at CMake time. Test bodies stay
// platform-independent and only ever include this header.

namespace OmegaGTETests {

    /// Backend-neutral window spec. Every field maps 1:1 to something every
    /// toolkit (Win32, Cocoa, GTK, Wayland, Android) can honor.
    struct GTETestWindowDescriptor {
        const char  *title  = "GTE Test";
        unsigned     width  = 500;
        unsigned     height = 500;
        OmegaGTE::PixelFormat pixelFormat = OmegaGTE::PixelFormat::BGRA8Unorm;
        bool         allowDepthStencilTesting = false;

        /// Optional: capture the first rendered frame to PNG and exit.
        /// Empty (default) = interactive window. Hookup is deferred to Phase 5
        /// of the cross-backend plan — Phase 1 ignores this field but reserves
        /// the slot so the API does not change later.
        std::string  captureFramePath;
    };

    /// Lifecycle callbacks. The runtime guarantees all three fire on the GUI
    /// thread, in this order:
    ///   onReady  — once, after the window is realized and the
    ///              NativeRenderTargetDescriptor is fully populated for the
    ///              active backend. Test code creates the render target inside
    ///              onReady and holds it.
    ///   onFrame  — every redraw event after onReady. Optional. Tests that
    ///              render once and never animate can leave it unset.
    ///   onClose  — once, just before the platform run loop returns to
    ///              RunGTETestWindow's caller. Test code resets every GE
    ///              SharedHandle here, in dependency order, then calls
    ///              OmegaGTE::Close(gte).
    struct GTETestWindowDelegate {
        std::function<void(const OmegaGTE::NativeRenderTargetDescriptor &)> onReady;
        std::function<void()> onFrame;
        std::function<void()> onClose;
    };

    /// Open the window per `desc`, run the platform main loop until the user
    /// closes it, and return the process exit code. Must be called from the
    /// thread that will become the GUI main thread (i.e. from int main /
    /// WinMain; not from a worker).
    int RunGTETestWindow(int argc,
                         const char *argv[],
                         const GTETestWindowDescriptor &desc,
                         const GTETestWindowDelegate &delegate);

    /// Ends the run loop and has RunGTETestWindow return `exitCode`, as if the
    /// user had closed the window — onClose still fires first. For tests that
    /// render (or compute) once, decide pass/fail, and exit without waiting on
    /// user interaction (e.g. GPUTessTest / CPUTessTest), call this at the end
    /// of onReady. Must be called on the GUI thread, after RunGTETestWindow has
    /// been entered (i.e. from onReady or onFrame, not before).
    void RequestGTETestWindowClose(int exitCode);

} // namespace OmegaGTETests

#endif // OMEGAGTE_TESTS_GTETESTWINDOW_H
