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


// GTK 4 retired GdkScreen and the global DPI resolution, the indexed monitor
// accessors (replaced by gdk_display_get_monitors() → GListModel), the
// primary-monitor concept, and gdk_monitor_get_workarea. DPI is folded into the
// per-monitor scale (gdk_monitor_get_scale_factor), so the DIP divisor is 1, the
// combined scale is just the monitor scale, visibleFrame == frame, and the first
// monitor stands in for "primary".

bool fillMonitorDesc(GdkMonitor *m, unsigned id, NativeScreenDesc & d) {
    if(m == nullptr) return false;

    GdkRectangle geom{};
    gdk_monitor_get_geometry(m, &geom);
    gint gdkScale = gdk_monitor_get_scale_factor(m);
    if(gdkScale < 1) gdkScale = 1;

    auto toRect = [](const GdkRectangle & r) -> Composition::Rect {
        return Composition::Rect{
            Composition::Point2D{(float)r.x, (float)r.y},
            (float)r.width, (float)r.height};
    };

    d.id           = id;
    d.frame        = toRect(geom);
    d.visibleFrame = toRect(geom);     // GTK 4 has no workarea concept
    d.scaleFactor  = (float)gdkScale;
    d.isPrimary    = (id == 0);        // GTK 4 has no primary-monitor notion

    int millihz = gdk_monitor_get_refresh_rate(m);
    d.refreshHz = millihz > 0 ? (float)millihz / 1000.f : 60.f;
    d.minRefreshHz        = d.refreshHz;
    d.variableRefreshRate = false;
    return true;
}

// get_item returns a new ref; the GListModel keeps the monitors alive for the
// display's lifetime, so we drop our ref and hand back a borrowed pointer
// (matching GTK 3's gdk_display_get_monitor borrow semantics).
GdkMonitor * monitorByIndex(unsigned idx) {
    GdkDisplay *display = gdk_display_get_default();
    if(display == nullptr) return nullptr;
    GListModel *monitors = gdk_display_get_monitors(display);
    if(monitors == nullptr) return nullptr;
    if(idx >= g_list_model_get_n_items(monitors)) return nullptr;
    GdkMonitor *m = GDK_MONITOR(g_list_model_get_item(monitors, idx));
    if(m != nullptr) g_object_unref(m);
    return m;
}

GdkMonitor * primaryMonitor() { return monitorByIndex(0); }


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
    GListModel *monitors = gdk_display_get_monitors(display);
    if(monitors == nullptr) return out;
    guint n = g_list_model_get_n_items(monitors);
    for(guint i = 0; i < n; i++){
        GdkMonitor *m = GDK_MONITOR(g_list_model_get_item(monitors, i));
        NativeScreenDesc d;
        if(GTK::fillMonitorDesc(m, i, d)){
            out.push_back(d);
        }
        if(m != nullptr) g_object_unref(m);
    }
    return out;
}

NativeScreenDesc primaryScreen() {
    NativeScreenDesc d;
    GdkMonitor *m = GTK::primaryMonitor();   // first monitor (no GTK 4 primary)
    if(m == nullptr) return d;
    GTK::fillMonitorDesc(m, 0, d);
    return d;
}

NativeScreenDesc screenById(unsigned id) {
    NativeScreenDesc d;
    GdkMonitor *m = GTK::monitorByIndex(id);
    if(m == nullptr){
        m = GTK::primaryMonitor();
        if(m == nullptr) return d;
        id = 0;
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
