#ifndef OMEGAWTK_NATIVE_NATIVETIMER_H
#define OMEGAWTK_NATIVE_NATIVETIMER_H

#include "omegaWTK/Core/Core.h"

#include <functional>

namespace OmegaWTK::Native {

    /// A run-loop timer hosted by the OS event loop.
    ///
    /// Threading: the callback fires on the **main / UI thread** in
    /// every backend (`NSTimer` on `[NSRunLoop mainRunLoop]`, Win32
    /// `WM_TIMER` dispatched by the message pump, `g_timeout_add` on
    /// the default `GMainContext`). Background-thread timers are
    /// intentionally out of scope for §2.4 v0 — that's a separate
    /// API surface if it's ever wanted.
    ///
    /// Lifetime: `make_native_timer` returns a shared handle that owns
    /// the underlying OS timer. The timer is started immediately on
    /// construction; `stop()` cancels it. Dropping the last shared
    /// reference also stops the timer.
    ///
    /// Repeating vs one-shot: when `repeats == false`, the timer stops
    /// itself after firing once — `isRunning()` returns `false`
    /// immediately after the callback returns. When `repeats == true`,
    /// the timer continues firing at the configured interval until
    /// `stop()` is called or the handle is released.
    INTERFACE NativeTimer {
    public:
        /// Start (or restart) the timer. The next fire happens
        /// `intervalSec` seconds from this call. Calling on a running
        /// timer is a restart.
        virtual void start() = 0;

        /// Stop the timer. The callback will not fire again until
        /// `start()` is called. Safe to call when already stopped.
        virtual void stop() = 0;

        /// True iff the timer is currently scheduled to fire.
        virtual bool isRunning() const = 0;

        virtual ~NativeTimer() = default;
    };
    typedef SharedHandle<NativeTimer> NativeTimerPtr;

    /// Construct a timer scheduled on the main run-loop. The timer is
    /// started immediately. `intervalSec` is the seconds between
    /// successive fires (or, for `repeats == false`, the delay before
    /// the single fire). `callback` is invoked on the UI thread.
    OMEGAWTK_EXPORT NativeTimerPtr make_native_timer(float intervalSec,
                                                      bool repeats,
                                                      std::function<void()> callback);

}

#endif
