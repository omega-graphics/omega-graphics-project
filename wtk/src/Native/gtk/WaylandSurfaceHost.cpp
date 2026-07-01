#include "NativePrivate/gtk/WaylandSurfaceHost.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace OmegaWTK::Native::GTK {

namespace {
#if WTK_NATIVE_WAYLAND
static int toWaylandCoordinate(float value){
    if(!std::isfinite(value)){
        return 0;
    }
    return static_cast<int>(std::lround(value));
}
#endif
}

#if WTK_NATIVE_WAYLAND
WaylandSurfaceHost::WaylandSurfaceHost(wl_display *dpy,
                                       wl_compositor *comp,
                                       wl_subcompositor *subcomp):
SurfaceHost(WindowingBackend::Wayland),
dpy_(dpy),
comp_(comp),
subcomp_(subcomp){
}
#else
WaylandSurfaceHost::WaylandSurfaceHost():
SurfaceHost(WindowingBackend::Wayland){
}
#endif

WaylandSurfaceHost::~WaylandSurfaceHost(){
#if WTK_NATIVE_WAYLAND
    // Reverse-iterate so the destroy sequence is opposite to creation,
    // matching X11SurfaceHost's deterministic tear-down. The subsurface
    // must be destroyed before its backing wl_surface.
    for(auto it = childOrder_.rbegin(); it != childOrder_.rend(); ++it){
        if(it->subsurface != nullptr){
            wl_subsurface_destroy(it->subsurface);
        }
        if(it->surface != nullptr){
            wl_surface_destroy(it->surface);
        }
    }
    childOrder_.clear();
    if(dpy_ != nullptr){
        wl_display_flush(dpy_);
    }
#endif
}

#if WTK_NATIVE_WAYLAND
void WaylandSurfaceHost::onToplevelRealized(wl_surface *toplevelSurface){
    std::vector<std::function<void()>> drain;
    {
        std::lock_guard<std::mutex> lk(realizeMutex_);
        toplevel_ = toplevelSurface;
        const bool firstRealize = !realized_;
        realized_ = true;
        if(firstRealize){
            // Drain + free storage: subscribers fire once. Subsequent
            // realize cycles (a forced surface recreate) re-cache the
            // handle but do not replay the queue. Identical to X11.
            drain.swap(pendingRealize_);
        }
    }
    for(auto &cb : drain){
        if(cb) cb();
    }
}

wl_surface *WaylandSurfaceHost::createChildSurface(const Composition::Rect &rect){
    if(dpy_ == nullptr || toplevel_ == nullptr ||
       comp_ == nullptr || subcomp_ == nullptr){
        return nullptr;
    }
    wl_surface *child = wl_compositor_create_surface(comp_);
    if(child == nullptr){
        return nullptr;
    }
    wl_subsurface *sub = wl_subcompositor_get_subsurface(subcomp_, child, toplevel_);
    if(sub == nullptr){
        wl_surface_destroy(child);
        return nullptr;
    }
    // Desync mode lets a content node commit independently of the toplevel.
    wl_subsurface_set_desync(sub);
    wl_subsurface_set_position(sub,
        toWaylandCoordinate(rect.pos.x),
        toWaylandCoordinate(rect.pos.y));
    // Child extent is dictated by the buffer the content node attaches, not
    // by a protocol call here — Vulkan WSI on Wayland reports no surface
    // extent, so the swap chain is sized from the §2.14 descriptor's
    // width/height (§2.13a). rect.w/rect.h carry that size to the descriptor;
    // the subsurface itself only positions and stacks.
    childOrder_.push_back(ChildSurface{child, sub, 0});
    // set_position/stacking are double-buffered on the parent's pending
    // state and apply on the parent commit. The first-map cadence between a
    // desync subsurface and the parent commit is a Pass-2 concern
    // (§2.13a risk 2); the present-but-unused registry just issues the commit.
    wl_surface_commit(toplevel_);
    wl_display_flush(dpy_);
    return child;
}

void WaylandSurfaceHost::destroyChildSurface(wl_surface *child){
    if(dpy_ == nullptr || child == nullptr){
        return;
    }
    auto it = std::find_if(childOrder_.begin(), childOrder_.end(),
        [child](const ChildSurface &s){ return s.surface == child; });
    if(it == childOrder_.end()){
        return;
    }
    if(it->subsurface != nullptr){
        wl_subsurface_destroy(it->subsurface);
    }
    wl_surface_destroy(it->surface);
    childOrder_.erase(it);
    if(toplevel_ != nullptr){
        // Reflect the removal in the parent's surface tree.
        wl_surface_commit(toplevel_);
    }
    wl_display_flush(dpy_);
}

void WaylandSurfaceHost::reconfigureChildSurface(wl_surface *child,
                                                 const Composition::Rect &rect,
                                                 int zOrder){
    if(dpy_ == nullptr || child == nullptr){
        return;
    }
    auto it = std::find_if(childOrder_.begin(), childOrder_.end(),
        [child](const ChildSurface &s){ return s.surface == child; });
    if(it == childOrder_.end()){
        return;
    }
    wl_subsurface_set_position(it->subsurface,
        toWaylandCoordinate(rect.pos.x),
        toWaylandCoordinate(rect.pos.y));
    it->zOrder = zOrder;
    restackChildren();
    if(toplevel_ != nullptr){
        wl_surface_commit(toplevel_);
    }
    wl_display_flush(dpy_);
}

void WaylandSurfaceHost::restackChildren(){
    if(dpy_ == nullptr || toplevel_ == nullptr || childOrder_.size() < 2){
        return;
    }
    // Sort ascending by zOrder — Wayland stacks bottom-up. Stable sort keeps
    // creation order as a tiebreaker so equal-z children don't flicker on
    // every reconfigure. Place the lowest just above the parent toplevel,
    // then each subsequent child just above the previous one, which yields a
    // total order with the highest zOrder on top.
    std::vector<ChildSurface> sorted = childOrder_;
    std::stable_sort(sorted.begin(), sorted.end(),
        [](const ChildSurface &a, const ChildSurface &b){
            return a.zOrder < b.zOrder;
        });
    wl_surface *below = toplevel_;
    for(const auto &s : sorted){
        if(s.subsurface != nullptr){
            wl_subsurface_place_above(s.subsurface, below);
            below = s.surface;
        }
    }
}
#endif

bool WaylandSurfaceHost::isRealized() const {
    std::lock_guard<std::mutex> lk(realizeMutex_);
    return realized_;
}

void WaylandSurfaceHost::runOnRealize(std::function<void()> action){
    if(!action){
        return;
    }
    {
        std::lock_guard<std::mutex> lk(realizeMutex_);
        if(!realized_){
            pendingRealize_.push_back(std::move(action));
            return;
        }
    }
    // Already realized — fire synchronously. Lock released before the call
    // so subscribers can re-enter WaylandSurfaceHost without deadlock.
    action();
}

void WaylandSurfaceHost::reconfigureChild(std::uint64_t hostId,
                                          const Composition::Rect &rectPx,
                                          int zOrder){
#if WTK_NATIVE_WAYLAND
    auto it = hostChildren_.find(hostId);
    wl_surface *child = (it != hostChildren_.end()) ? it->second : nullptr;
    if(child == nullptr){
        // First reconfigure for this hostId allocates the child subsurface.
        // Returns null if the toplevel is not yet realized (or no
        // compositor/subcompositor was supplied) — leave the mapping unset so
        // the caller's runOnRealize retry re-allocates.
        child = createChildSurface(rectPx);
        if(child == nullptr){
            return;
        }
        hostChildren_.emplace(hostId, child);
    }
    reconfigureChildSurface(child, rectPx, zOrder);
#else
    (void)hostId;
    (void)rectPx;
    (void)zOrder;
#endif
}

void WaylandSurfaceHost::destroyChild(std::uint64_t hostId){
#if WTK_NATIVE_WAYLAND
    auto it = hostChildren_.find(hostId);
    if(it == hostChildren_.end()){
        return;
    }
    destroyChildSurface(it->second);
    hostChildren_.erase(it);
#else
    (void)hostId;
#endif
}

}
