#ifndef OMEGAWTK_NATIVE_NATIVESCREEN_H
#define OMEGAWTK_NATIVE_NATIVESCREEN_H

#include "omegaWTK/Core/Core.h"
#include "omegaWTK/Composition/Geometry.h"

#include <cstdint>
#include <functional>

namespace OmegaWTK::Native {

    /// Identity + geometry of a connected display.
    ///
    /// Lifetime: snapshot at enumeration time. `id` is stable across
    /// repeated `enumerateScreens()` calls within the same session as
    /// long as the OS reports the same display set. After display
    /// hot-plug / reconfigure the safest contract is to re-enumerate
    /// and re-resolve; `screenById` resolves to `primaryScreen()` for
    /// unknown ids so callers always have a valid target.
    struct NativeScreenDesc {
        /// Opaque per-screen identifier. macOS: CGDirectDisplayID.
        /// Win32: HMONITOR cast to unsigned. GTK: monitor index into
        /// gdk_display_get_n_monitors at enumeration time.
        unsigned id = 0;

        /// Virtual-screen coordinates in DIPs (logical pixels). The
        /// origin and orientation match the platform's own virtual
        /// screen convention (top-left on Win32/GTK, bottom-left on
        /// macOS at the NSScreen layer — translated to top-left here
        /// so all three backends report uniform coordinates).
        Composition::Rect frame {{0.f, 0.f}, 0.f, 0.f};

        /// `frame` minus reserved areas (menu bars, docks, taskbars,
        /// panels). What an app should use to size content windows.
        Composition::Rect visibleFrame {{0.f, 0.f}, 0.f, 0.f};

        /// Combined logical → physical scale (matches
        /// `NativeWindow::scaleFactor()` for a window currently on
        /// this screen). macOS: NSScreen.backingScaleFactor. Win32:
        /// GetDpiForMonitor / 96. GTK: gdk_screen_get_resolution / 96
        /// × gdk_monitor_get_scale_factor.
        float scaleFactor = 1.f;

        /// True for the OS-designated primary display.
        bool isPrimary = false;

        /// Refresh-rate descriptor. Variable-refresh displays
        /// (ProMotion / G-Sync / FreeSync) report their maximum here;
        /// the actual per-frame interval is delivered through the
        /// display link.
        ///
        /// Fixed-rate displays: the rate. VRR displays: max rate.
        float refreshHz = 60.f;

        /// VRR floor. Equals `refreshHz` on fixed-rate displays.
        /// Reported only on backends that surface a VRR query —
        /// Win32/GTK first-cut leave this equal to `refreshHz`.
        float minRefreshHz = 60.f;

        /// True iff the OS reports VRR for this display.
        /// Win32/GTK first-cut report `false`; macOS reports the real
        /// value via NSScreen's frame-rate window.
        bool variableRefreshRate = false;
    };

    /// Enumerate every connected display.
    OMEGAWTK_EXPORT OmegaCommon::Vector<NativeScreenDesc> enumerateScreens();

    /// The OS-designated primary display.
    OMEGAWTK_EXPORT NativeScreenDesc primaryScreen();

    /// Resolve a screen by id. Returns `primaryScreen()` when id is
    /// unknown so callers always have a valid target — no exceptions,
    /// no null sentinels. The "did the screen go away" check is a
    /// re-enumeration, not an error path here.
    OMEGAWTK_EXPORT NativeScreenDesc screenById(unsigned id);

    /// Per-screen vsync source. One platform display-link object per
    /// physical display; multiple consumers on the same screen share
    /// the same `NativeDisplayLink` instance (cached by screen id in
    /// the backend impl).
    ///
    /// Single-subscriber: `subscribe` replaces the previous callback.
    /// Phase H's FramePacer is the only named consumer; a hypothetical
    /// second consumer is a non-breaking change behind this surface.
    ///
    /// Threading: the callback fires on the platform's display-link
    /// thread (NOT the UI thread). Consumers (FramePacer) marshal to
    /// the UI thread before running per-frame work.
    INTERFACE NativeDisplayLink {
    public:
        /// Subscribe to vsync notifications for this screen. The
        /// callback fires on every refresh tick (or every variable-
        /// refresh delivery under VRR) with the predicted *next*
        /// presentation time and the measured interval since the
        /// previous fire, both in nanoseconds.
        ///
        /// Replaces any previous subscription. Pass an empty
        /// std::function (or call `unsubscribe`) to detach.
        virtual void subscribe(std::function<void(std::uint64_t presentationTimeNs,
                                                   std::uint64_t intervalNs)> cb) = 0;

        /// Detach the current subscriber. Safe to call when no
        /// subscriber is attached.
        virtual void unsubscribe() = 0;

        /// Current expected frame interval in nanoseconds.
        /// 16'666'666 @ 60 Hz, 8'333'333 @ 120 Hz. Under VRR returns
        /// the most-recently-measured interval.
        virtual std::uint64_t expectedFrameIntervalNs() const = 0;

        virtual ~NativeDisplayLink() = default;
    };
    typedef SharedHandle<NativeDisplayLink> NativeDisplayLinkPtr;

    /// Acquire the display link for `screen`. Multiple calls for the
    /// same screen return the same shared instance — the backend
    /// caches by `NativeScreenDesc::id`. The cache entry is released
    /// when the last consumer's `NativeDisplayLinkPtr` drops.
    OMEGAWTK_EXPORT NativeDisplayLinkPtr displayLinkForScreen(const NativeScreenDesc & screen);

}

#endif
