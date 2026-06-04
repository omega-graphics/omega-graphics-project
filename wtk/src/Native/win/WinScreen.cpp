#include "omegaWTK/Native/NativeScreen.h"

#include <Windows.h>
#include <dxgi.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace OmegaWTK::Native::Win {

namespace {

/// Squash a HMONITOR (a 64-bit kernel handle on x64) into the 32-bit
/// `unsigned` `NativeScreenDesc::id` field. We fold high and low
/// halves so monitor handles whose unique bits live in the upper
/// word still produce distinct ids. Lookup walks the live monitor
/// set and matches by re-hash, so this only has to be collision-free
/// across the live set at one enumeration time.
unsigned hmonitorToId(HMONITOR h) {
    auto p = reinterpret_cast<std::uintptr_t>(h);
#if INTPTR_MAX > 0xFFFFFFFFll
    return (unsigned)(p ^ (p >> 32));
#else
    return (unsigned)p;
#endif
}

struct MonitorEnumCtx {
    std::vector<HMONITOR> monitors;
};

BOOL CALLBACK monitorEnumProc(HMONITOR h, HDC, LPRECT, LPARAM lparam) {
    auto *ctx = reinterpret_cast<MonitorEnumCtx *>(lparam);
    ctx->monitors.push_back(h);
    return TRUE;
}

std::vector<HMONITOR> enumerateMonitors() {
    MonitorEnumCtx ctx;
    EnumDisplayMonitors(nullptr, nullptr, &monitorEnumProc,
                         reinterpret_cast<LPARAM>(&ctx));
    return ctx.monitors;
}

HMONITOR primaryHMonitor() {
    POINT origin{0, 0};
    return MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
}

HMONITOR findMonitorById(unsigned id) {
    for(HMONITOR h : enumerateMonitors()){
        if(hmonitorToId(h) == id){
            return h;
        }
    }
    return nullptr;
}

/// Dynamically load Shcore!GetDpiForMonitor so OmegaWTK_Native does
/// not need a link-time dependency on Shcore.lib. Resolved once per
/// process; the DLL handle leaks intentionally.
using PFN_GetDpiForMonitor = HRESULT (WINAPI *)(HMONITOR, int, UINT *, UINT *);

PFN_GetDpiForMonitor loadGetDpiForMonitor() {
    static PFN_GetDpiForMonitor fn = nullptr;
    static std::once_flag once;
    std::call_once(once, []{
        HMODULE dll = LoadLibraryW(L"Shcore.dll");
        if(dll != nullptr){
            fn = reinterpret_cast<PFN_GetDpiForMonitor>(
                GetProcAddress(dll, "GetDpiForMonitor"));
        }
    });
    return fn;
}

float dpiScaleForMonitor(HMONITOR h) {
    // MDT_EFFECTIVE_DPI = 0 — the OS scaling for this monitor.
    auto fn = loadGetDpiForMonitor();
    if(fn != nullptr){
        UINT dpiX = 96, dpiY = 96;
        if(SUCCEEDED(fn(h, 0 /*MDT_EFFECTIVE_DPI*/, &dpiX, &dpiY)) && dpiX > 0){
            return (float)dpiX / 96.f;
        }
    }
    // Fallback: system DPI. Wrong on per-monitor mixed-DPI setups,
    // but matches what GetDpiForWindow returns on a non-DPI-aware
    // process.
    HDC dc = GetDC(nullptr);
    if(dc != nullptr){
        int dpi = GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(nullptr, dc);
        if(dpi > 0) return (float)dpi / 96.f;
    }
    return 1.f;
}

float refreshHzForMonitor(const MONITORINFOEXW & info) {
    DEVMODEW dev{};
    dev.dmSize = sizeof(dev);
    if(!EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &dev)){
        return 60.f;
    }
    // 0 / 1 are "default rate" sentinels.
    if(dev.dmDisplayFrequency <= 1) return 60.f;
    return (float)dev.dmDisplayFrequency;
}

bool fillMonitorDesc(HMONITOR h, NativeScreenDesc & d) {
    if(h == nullptr) return false;
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if(!GetMonitorInfoW(h, reinterpret_cast<MONITORINFO *>(&info))){
        return false;
    }
    float scale = dpiScaleForMonitor(h);
    auto toRect = [scale](const RECT & r) -> Composition::Rect {
        return Composition::Rect{
            Composition::Point2D{(float)r.left / scale, (float)r.top / scale},
            (float)(r.right - r.left) / scale,
            (float)(r.bottom - r.top) / scale};
    };
    d.id           = hmonitorToId(h);
    d.frame        = toRect(info.rcMonitor);
    d.visibleFrame = toRect(info.rcWork);
    d.scaleFactor  = scale;
    d.isPrimary    = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    d.refreshHz    = refreshHzForMonitor(info);
    // VRR not detected in this first cut. Win32 needs
    // IDXGIOutput6::CheckHardwareCompositionSupport which pulls
    // dxgi1_6 into the call site — deferred to a follow-up.
    d.minRefreshHz         = d.refreshHz;
    d.variableRefreshRate  = false;
    return true;
}

/// Dynamic dxgi.dll loader. Same rationale as Shcore — keeps
/// OmegaWTK_Native's link surface unchanged.
using PFN_CreateDXGIFactory1 = HRESULT (WINAPI *)(REFIID, void **);

PFN_CreateDXGIFactory1 loadCreateDXGIFactory1() {
    static PFN_CreateDXGIFactory1 fn = nullptr;
    static std::once_flag once;
    std::call_once(once, []{
        HMODULE dll = LoadLibraryW(L"dxgi.dll");
        if(dll != nullptr){
            fn = reinterpret_cast<PFN_CreateDXGIFactory1>(
                GetProcAddress(dll, "CreateDXGIFactory1"));
        }
    });
    return fn;
}

/// Walk DXGI adapters and outputs to find the one whose
/// DXGI_OUTPUT_DESC::Monitor matches `target`. Returns nullptr if
/// dxgi is unavailable or no output matches. Caller owns the
/// returned ref (release via output->Release()).
IDXGIOutput * findDXGIOutputForMonitor(HMONITOR target) {
    auto pCreateFactory = loadCreateDXGIFactory1();
    if(pCreateFactory == nullptr || target == nullptr) return nullptr;

    IDXGIFactory1 *factory = nullptr;
    if(FAILED(pCreateFactory(__uuidof(IDXGIFactory1),
                              reinterpret_cast<void **>(&factory)))
       || factory == nullptr){
        return nullptr;
    }

    IDXGIOutput *found = nullptr;
    UINT ai = 0;
    IDXGIAdapter1 *adapter = nullptr;
    while(factory->EnumAdapters1(ai++, &adapter) != DXGI_ERROR_NOT_FOUND){
        UINT oi = 0;
        IDXGIOutput *output = nullptr;
        while(adapter->EnumOutputs(oi++, &output) != DXGI_ERROR_NOT_FOUND){
            DXGI_OUTPUT_DESC desc{};
            if(SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == target){
                found = output;
                break;
            }
            output->Release();
            output = nullptr;
        }
        adapter->Release();
        adapter = nullptr;
        if(found != nullptr) break;
    }
    factory->Release();
    return found;
}

}

/// IDXGIOutput::WaitForVBlank pacer thread. One thread per shared
/// display link; the thread runs only while a subscriber is
/// attached.
///
/// Threading: the cb fires on the pacer thread (NOT the UI thread),
/// matching NativeScreen.h's contract. Phase H's FramePacer marshals
/// to the UI thread before running FrameBuilder.
class WinDisplayLink : public NativeDisplayLink {
public:
    explicit WinDisplayLink(HMONITOR hmonitor)
        : monitor_(hmonitor) {
        output_ = findDXGIOutputForMonitor(monitor_);

        // Seed nominal interval from the monitor's current refresh
        // mode so expectedFrameIntervalNs() has a sensible value
        // before the first vsync tick.
        if(monitor_ != nullptr){
            MONITORINFOEXW info{};
            info.cbSize = sizeof(info);
            if(GetMonitorInfoW(monitor_, reinterpret_cast<MONITORINFO *>(&info))){
                float hz = refreshHzForMonitor(info);
                if(hz > 0.f){
                    nominalIntervalNs_ =
                        (std::uint64_t)(1'000'000'000.0 / (double)hz);
                }
            }
        }

        QueryPerformanceFrequency(&qpcFreq_);
        if(qpcFreq_.QuadPart == 0){
            qpcFreq_.QuadPart = 1;
        }
    }

    ~WinDisplayLink() override {
        unsubscribe();
        if(output_ != nullptr){
            output_->Release();
            output_ = nullptr;
        }
    }

    void subscribe(std::function<void(std::uint64_t, std::uint64_t)> cb) override {
        {
            std::lock_guard<std::mutex> lk(mu_);
            callback_ = std::move(cb);
        }
        bool already = running_.exchange(true);
        if(callback_ && !already){
            stop_.store(false);
            thread_ = std::thread(&WinDisplayLink::pacerLoop, this);
        } else if(!callback_ && already){
            // subscribe(empty) is the documented "detach" form.
            stopThread();
        }
    }

    void unsubscribe() override {
        stopThread();
        std::lock_guard<std::mutex> lk(mu_);
        callback_ = nullptr;
    }

    std::uint64_t expectedFrameIntervalNs() const override {
        std::lock_guard<std::mutex> lk(mu_);
        return nominalIntervalNs_;
    }

private:
    void stopThread() {
        if(running_.exchange(false)){
            stop_.store(true);
            if(thread_.joinable()){
                thread_.join();
            }
        }
    }

    void pacerLoop() {
        while(!stop_.load(std::memory_order_relaxed)){
            // Block until vsync. If the DXGI output is unavailable
            // (no adapter found, dxgi missing), fall back to a sleep
            // at the nominal interval so the API still ticks.
            if(output_ != nullptr){
                output_->WaitForVBlank();
            } else {
                std::uint64_t ms = nominalIntervalNs_ / 1'000'000ull;
                if(ms == 0) ms = 1;
                Sleep((DWORD)ms);
            }
            if(stop_.load(std::memory_order_relaxed)){
                break;
            }

            LARGE_INTEGER qpc{};
            QueryPerformanceCounter(&qpc);
            std::uint64_t presentationNs = (std::uint64_t)(
                (double)qpc.QuadPart * 1'000'000'000.0
                / (double)qpcFreq_.QuadPart);

            std::function<void(std::uint64_t, std::uint64_t)> cb;
            std::uint64_t interval;
            {
                std::lock_guard<std::mutex> lk(mu_);
                cb = callback_;
                interval = nominalIntervalNs_;
            }
            if(cb){
                cb(presentationNs, interval);
            }
        }
    }

    HMONITOR monitor_;
    IDXGIOutput *output_ = nullptr;
    LARGE_INTEGER qpcFreq_{};
    std::function<void(std::uint64_t, std::uint64_t)> callback_;
    mutable std::mutex mu_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::uint64_t nominalIntervalNs_ = 16'666'666ull;
};

namespace {

struct DisplayLinkCache {
    std::mutex mu;
    std::unordered_map<unsigned, std::weak_ptr<NativeDisplayLink>> entries;
};

DisplayLinkCache & displayLinkCache() {
    static DisplayLinkCache cache;
    return cache;
}

}

}

namespace OmegaWTK::Native {

OmegaCommon::Vector<NativeScreenDesc> enumerateScreens() {
    OmegaCommon::Vector<NativeScreenDesc> out;
    for(HMONITOR h : Win::enumerateMonitors()){
        NativeScreenDesc d;
        if(Win::fillMonitorDesc(h, d)){
            out.push_back(d);
        }
    }
    return out;
}

NativeScreenDesc primaryScreen() {
    NativeScreenDesc d;
    Win::fillMonitorDesc(Win::primaryHMonitor(), d);
    return d;
}

NativeScreenDesc screenById(unsigned id) {
    NativeScreenDesc d;
    HMONITOR h = Win::findMonitorById(id);
    if(h == nullptr){
        h = Win::primaryHMonitor();
    }
    Win::fillMonitorDesc(h, d);
    return d;
}

NativeDisplayLinkPtr displayLinkForScreen(const NativeScreenDesc & screen) {
    auto & cache = Win::displayLinkCache();
    std::lock_guard<std::mutex> lk(cache.mu);
    auto it = cache.entries.find(screen.id);
    if(it != cache.entries.end()){
        if(auto live = it->second.lock()){
            return live;
        }
        cache.entries.erase(it);
    }
    HMONITOR h = Win::findMonitorById(screen.id);
    if(h == nullptr){
        h = Win::primaryHMonitor();
    }
    auto link = std::make_shared<Win::WinDisplayLink>(h);
    cache.entries.emplace(screen.id, link);
    return link;
}

}
