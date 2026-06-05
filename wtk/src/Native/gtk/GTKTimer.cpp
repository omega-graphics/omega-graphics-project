#include "omegaWTK/Native/NativeTimer.h"

#include <glib.h>

#include <memory>
#include <utility>

namespace OmegaWTK::Native::GTK {

/// g_timeout_add-backed timer on the default GMainContext. The
/// GMainLoop driving GtkApplication's event loop ticks this on the
/// UI thread per `NativeTimer.h`'s contract.
class GTKTimer : public NativeTimer {
public:
    GTKTimer(float intervalSec, bool repeats, std::function<void()> callback)
        : intervalMs_(intervalSec > 0.f ? (guint)(intervalSec * 1000.0f) : 0u),
          repeats_(repeats),
          callback_(std::move(callback)) {
        scheduleLocked();
    }

    ~GTKTimer() override {
        cancelLocked();
    }

    void start() override {
        cancelLocked();
        scheduleLocked();
    }

    void stop() override {
        cancelLocked();
    }

    bool isRunning() const override {
        return sourceId_ != 0;
    }

    /// Invoked from the GSource thunk. One-shots clear `sourceId_`
    /// before invoking the callback so `isRunning()` reflects the
    /// post-fire state. Returns true to keep firing (repeats), false
    /// to remove the source (one-shot).
    gboolean onFire() {
        std::function<void()> cb = callback_;
        if(!repeats_){
            // GLib will remove the source after we return G_SOURCE_REMOVE.
            sourceId_ = 0;
        }
        if(cb){
            cb();
        }
        return repeats_ ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
    }

private:
    static gboolean sourceThunk(gpointer data){
        auto *self = static_cast<GTKTimer *>(data);
        return self->onFire();
    }

    void scheduleLocked() {
        sourceId_ = g_timeout_add(intervalMs_, &GTKTimer::sourceThunk, this);
    }

    void cancelLocked() {
        if(sourceId_ != 0){
            g_source_remove(sourceId_);
            sourceId_ = 0;
        }
    }

    guint intervalMs_;
    bool repeats_;
    std::function<void()> callback_;
    guint sourceId_ = 0;
};

}

namespace OmegaWTK::Native {
    NativeTimerPtr make_native_timer(float intervalSec,
                                      bool repeats,
                                      std::function<void()> callback){
        return std::make_shared<GTK::GTKTimer>(intervalSec, repeats, std::move(callback));
    }
}
