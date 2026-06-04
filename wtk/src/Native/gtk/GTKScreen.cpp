#include "omegaWTK/Native/NativeScreen.h"

#include <gtk/gtk.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace OmegaWTK::Native::GTK {

namespace {

/// dpiScale captures the resolution component (Xft.dpi / GDK_DPI_SCALE
/// / GNOME text-scaling-factor) — the part GTK does NOT auto-apply
/// to GdkRectangle geometry. Matches GTKAppWindow::computeDpiScale.
float computeDpiScale() {
    GdkDisplay *display = gdk_display_get_default();
    if(display == nullptr) return 1.f;
    GdkScreen *screen = gdk_display_get_default_screen(display);
    if(screen == nullptr) return 1.f;
    gdouble dpi = gdk_screen_get_resolution(screen);
    if(dpi <= 0.0 || !std::isfinite(dpi)) return 1.f;
    float scale = (float)(dpi / 96.0);
    return scale < 0.5f ? 0.5f : scale;
}

unsigned monitorIndex(GdkDisplay *display, GdkMonitor *m) {
    if(display == nullptr || m == nullptr) return 0;
    int n = gdk_display_get_n_monitors(display);
    for(int i = 0; i < n; i++){
        if(gdk_display_get_monitor(display, i) == m){
            return (unsigned)i;
        }
    }
    return 0;
}

GdkMonitor * monitorByIndex(unsigned idx) {
    GdkDisplay *display = gdk_display_get_default();
    if(display == nullptr) return nullptr;
    int n = gdk_display_get_n_monitors(display);
    if((int)idx >= n) return nullptr;
    return gdk_display_get_monitor(display, (int)idx);
}

GdkMonitor * primaryMonitor() {
    GdkDisplay *display = gdk_display_get_default();
    if(display == nullptr) return nullptr;
    GdkMonitor *m = gdk_display_get_primary_monitor(display);
    if(m == nullptr){
        m = gdk_display_get_monitor(display, 0);
    }
    return m;
}

bool fillMonitorDesc(GdkMonitor *m, unsigned id, NativeScreenDesc & d) {
    if(m == nullptr) return false;

    GdkRectangle geom{};
    gdk_monitor_get_geometry(m, &geom);
    GdkRectangle work{};
    gdk_monitor_get_workarea(m, &work);

    float dpiScale = computeDpiScale();
    gint gdkScale = gdk_monitor_get_scale_factor(m);
    if(gdkScale < 1) gdkScale = 1;
    float combined = dpiScale * (float)gdkScale;

    // GdkRectangle is in GTK logical pixels (already divided by the
    // GDK integer scale). Divide by dpiScale to reach DIPs — matches
    // GTKAppWindow::fromGtkLogical so a window placed at NativeScreen
    // coords lines up with what the AppWindow ctor expects.
    float divisor = dpiScale > 0.f ? dpiScale : 1.f;
    auto toRect = [divisor](const GdkRectangle & r) -> Composition::Rect {
        return Composition::Rect{
            Composition::Point2D{(float)r.x / divisor, (float)r.y / divisor},
            (float)r.width  / divisor,
            (float)r.height / divisor};
    };

    d.id           = id;
    d.frame        = toRect(geom);
    d.visibleFrame = toRect(work);
    d.scaleFactor  = combined;

    GdkDisplay *display = gdk_monitor_get_display(m);
    d.isPrimary = display != nullptr
                  && gdk_display_get_primary_monitor(display) == m;

    // gdk_monitor_get_refresh_rate returns millihertz; 0 on
    // compositors that don't report it (some nested Wayland setups).
    int millihz = gdk_monitor_get_refresh_rate(m);
    d.refreshHz = millihz > 0 ? (float)millihz / 1000.f : 60.f;

    // VRR not exposed by GDK — first cut reports fixed-rate. A
    // future Wayland-VRR-protocol backend can opt in here.
    d.minRefreshHz        = d.refreshHz;
    d.variableRefreshRate = false;
    return true;
}

}

/// First-cut GTK display link: a GLib millisecond timer at the
/// screen's refresh interval. Fires on the GMainContext thread (the
/// GTK main thread), so consumers should treat the cb as on-UI-thread
/// — matching the macOS first cut. Phase H's FramePacer can rewire
/// to a wl_surface.frame / wp_presentation_feedback (Wayland) or
/// GLX_INTEL_swap_event (X11) source if vsync-accurate pacing is
/// required.
class GTKDisplayLink : public NativeDisplayLink {
public:
    explicit GTKDisplayLink(std::uint64_t intervalNs)
        : nominalIntervalNs_(intervalNs > 0 ? intervalNs : 16'666'666ull) {}

    ~GTKDisplayLink() override {
        std::lock_guard<std::mutex> lk(mu_);
        callback_ = nullptr;
        stopTimerLocked();
    }

    void subscribe(std::function<void(std::uint64_t, std::uint64_t)> cb) override {
        std::lock_guard<std::mutex> lk(mu_);
        callback_ = std::move(cb);
        if(callback_ && sourceId_ == 0){
            std::uint64_t ms = nominalIntervalNs_ / 1'000'000ull;
            if(ms == 0) ms = 1;
            sourceId_ = g_timeout_add((guint)ms, &GTKDisplayLink::tickThunk, this);
        } else if(!callback_){
            stopTimerLocked();
        }
    }

    void unsubscribe() override {
        std::lock_guard<std::mutex> lk(mu_);
        callback_ = nullptr;
        stopTimerLocked();
    }

    std::uint64_t expectedFrameIntervalNs() const override {
        std::lock_guard<std::mutex> lk(mu_);
        return nominalIntervalNs_;
    }

private:
    void stopTimerLocked() {
        if(sourceId_ != 0){
            g_source_remove(sourceId_);
            sourceId_ = 0;
        }
    }

    static gboolean tickThunk(gpointer data) {
        auto *self = static_cast<GTKDisplayLink *>(data);
        std::function<void(std::uint64_t, std::uint64_t)> cb;
        std::uint64_t intervalNs;
        {
            std::lock_guard<std::mutex> lk(self->mu_);
            cb = self->callback_;
            intervalNs = self->nominalIntervalNs_;
            if(!cb){
                self->sourceId_ = 0;
                return G_SOURCE_REMOVE;
            }
        }
        // g_get_monotonic_time returns microseconds since some
        // unspecified epoch — fine for "interval since prior fire" math.
        std::int64_t monoUs = g_get_monotonic_time();
        std::uint64_t presentationNs = (std::uint64_t)monoUs * 1000ull;
        cb(presentationNs, intervalNs);
        return G_SOURCE_CONTINUE;
    }

    mutable std::mutex mu_;
    std::function<void(std::uint64_t, std::uint64_t)> callback_;
    guint sourceId_ = 0;
    std::uint64_t nominalIntervalNs_;
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
    GdkDisplay *display = gdk_display_get_default();
    if(display == nullptr) return out;
    int n = gdk_display_get_n_monitors(display);
    for(int i = 0; i < n; i++){
        GdkMonitor *m = gdk_display_get_monitor(display, i);
        NativeScreenDesc d;
        if(GTK::fillMonitorDesc(m, (unsigned)i, d)){
            out.push_back(d);
        }
    }
    return out;
}

NativeScreenDesc primaryScreen() {
    NativeScreenDesc d;
    GdkMonitor *primary = GTK::primaryMonitor();
    if(primary == nullptr) return d;
    GdkDisplay *display = gdk_monitor_get_display(primary);
    GTK::fillMonitorDesc(primary, GTK::monitorIndex(display, primary), d);
    return d;
}

NativeScreenDesc screenById(unsigned id) {
    NativeScreenDesc d;
    GdkMonitor *m = GTK::monitorByIndex(id);
    if(m == nullptr){
        m = GTK::primaryMonitor();
        if(m == nullptr) return d;
        id = GTK::monitorIndex(gdk_monitor_get_display(m), m);
    }
    GTK::fillMonitorDesc(m, id, d);
    return d;
}

NativeDisplayLinkPtr displayLinkForScreen(const NativeScreenDesc & screen) {
    auto & cache = GTK::displayLinkCache();
    std::lock_guard<std::mutex> lk(cache.mu);
    auto it = cache.entries.find(screen.id);
    if(it != cache.entries.end()){
        if(auto live = it->second.lock()){
            return live;
        }
        cache.entries.erase(it);
    }
    std::uint64_t intervalNs = 16'666'666ull;
    if(screen.refreshHz > 0.f){
        intervalNs = (std::uint64_t)(1'000'000'000.0 / (double)screen.refreshHz);
    }
    auto link = std::make_shared<GTK::GTKDisplayLink>(intervalNs);
    cache.entries.emplace(screen.id, link);
    return link;
}

}
