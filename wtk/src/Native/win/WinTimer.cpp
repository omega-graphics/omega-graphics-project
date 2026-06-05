#include "omegaWTK/Native/NativeTimer.h"

#include <Windows.h>

#include <memory>
#include <mutex>
#include <unordered_map>

namespace OmegaWTK::Native::Win {

class WinTimer;

namespace {

/// Registry of running timers, keyed by the UINT_PTR id SetTimer
/// returns. The static TIMERPROC dispatches through this; we cannot
/// pass user data to SetTimer directly when HWND is NULL.
struct TimerRegistry {
    std::mutex mu;
    std::unordered_map<UINT_PTR, WinTimer *> entries;
};

TimerRegistry & registry(){
    static TimerRegistry r;
    return r;
}

}

/// Win32 SetTimer-backed timer. Uses `SetTimer(NULL, ...)` so it is
/// not tied to a window — `WM_TIMER` messages are dispatched by the
/// main message pump (`WinApp::runEventLoop`) on the UI thread.
class WinTimer : public NativeTimer {
public:
    WinTimer(float intervalSec, bool repeats, std::function<void()> callback)
        : intervalMs_(intervalSec > 0.f ? (UINT)(intervalSec * 1000.0f) : 0u),
          repeats_(repeats),
          callback_(std::move(callback)) {
        scheduleLocked();
    }

    ~WinTimer() override {
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
        return timerId_ != 0;
    }

    /// Called from the static TIMERPROC. One-shots auto-cancel before
    /// invoking the callback so `isRunning()` reflects the post-fire
    /// state and the user's callback can safely call `stop()` itself.
    void onFire() {
        std::function<void()> cb;
        if(!repeats_){
            cancelLocked();
            cb = callback_;
        } else {
            cb = callback_;
        }
        if(cb){
            cb();
        }
    }

private:
    static void CALLBACK TimerProc(HWND hwnd, UINT msg, UINT_PTR id, DWORD time){
        (void)hwnd; (void)msg; (void)time;
        WinTimer *self = nullptr;
        {
            auto & r = registry();
            std::lock_guard<std::mutex> lk(r.mu);
            auto it = r.entries.find(id);
            if(it != r.entries.end()){
                self = it->second;
            }
        }
        if(self != nullptr){
            self->onFire();
        }
    }

    void scheduleLocked() {
        UINT_PTR id = SetTimer(NULL, 0, intervalMs_, &WinTimer::TimerProc);
        if(id == 0){
            // SetTimer failed — caller will see isRunning() == false.
            return;
        }
        timerId_ = id;
        auto & r = registry();
        std::lock_guard<std::mutex> lk(r.mu);
        r.entries[id] = this;
    }

    void cancelLocked() {
        if(timerId_ == 0){
            return;
        }
        UINT_PTR id = timerId_;
        timerId_ = 0;
        KillTimer(NULL, id);
        auto & r = registry();
        std::lock_guard<std::mutex> lk(r.mu);
        r.entries.erase(id);
    }

    UINT intervalMs_;
    bool repeats_;
    std::function<void()> callback_;
    UINT_PTR timerId_ = 0;
};

}

namespace OmegaWTK::Native {
    NativeTimerPtr make_native_timer(float intervalSec,
                                      bool repeats,
                                      std::function<void()> callback){
        return std::make_shared<Win::WinTimer>(intervalSec, repeats, std::move(callback));
    }
}
