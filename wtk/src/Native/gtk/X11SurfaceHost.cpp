#include "NativePrivate/gtk/X11SurfaceHost.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace OmegaWTK::Native::GTK {

namespace {
#if WTK_NATIVE_X11
static int toX11Coordinate(float value){
    if(!std::isfinite(value)){
        return 0;
    }
    return static_cast<int>(std::lround(value));
}

static unsigned toX11Dimension(float value){
    if(!std::isfinite(value) || value <= 0.f){
        return 1u;
    }
    long rounded = std::lround(value);
    if(rounded < 1L){
        rounded = 1L;
    }
    return static_cast<unsigned>(rounded);
}
#endif
}

#if WTK_NATIVE_X11
X11SurfaceHost::X11SurfaceHost(Display *dpy):
SurfaceHost(WindowingBackend::X11),
dpy_(dpy){
}
#else
X11SurfaceHost::X11SurfaceHost():
SurfaceHost(WindowingBackend::X11){
}
#endif

X11SurfaceHost::~X11SurfaceHost(){
#if WTK_NATIVE_X11
    if(dpy_ != nullptr){
        // Reverse-iterate so XDestroyWindow sees children in the order
        // opposite to creation; X11 itself doesn't care, but it keeps
        // tear-down deterministic for any toolkit running on the same
        // Display that observes the resulting DestroyNotify sequence.
        for(auto it = childOrder_.rbegin(); it != childOrder_.rend(); ++it){
            XDestroyWindow(dpy_, it->window);
        }
        XFlush(dpy_);
        childOrder_.clear();
        ownedChildren_.clear();
    }
#endif
}

#if WTK_NATIVE_X11
void X11SurfaceHost::onToplevelRealized(::Window toplevel){
    std::vector<std::function<void()>> drain;
    {
        std::lock_guard<std::mutex> lk(realizeMutex_);
        toplevel_ = toplevel;
        const bool firstRealize = !realized_;
        realized_ = true;
        if(firstRealize){
            // Drain + free storage: subscribers fire once. Subsequent
            // realize cycles (XID changes from a forced surface
            // recreate) re-cache the XID but do not replay the queue.
            drain.swap(pendingRealize_);
        }
    }
    for(auto &cb : drain){
        if(cb) cb();
    }
}

::Window X11SurfaceHost::createChildWindow(const Composition::Rect &rect){
    if(dpy_ == nullptr || toplevel_ == 0){
        return 0;
    }
    int screen = DefaultScreen(dpy_);
    ::Window root = RootWindow(dpy_, screen);
    (void)root;
    XSetWindowAttributes attrs{};
    // Inherit visual + colormap from the toplevel — child windows of
    // foreign-renderer surfaces must share the toplevel's visual so
    // X server compositing treats them consistently.
    attrs.background_pixmap = None;
    attrs.border_pixel = 0;
    attrs.event_mask = 0;
    unsigned long valueMask = CWBackPixmap | CWBorderPixel | CWEventMask;
    ::Window child = XCreateWindow(
        dpy_,
        toplevel_,
        toX11Coordinate(rect.pos.x),
        toX11Coordinate(rect.pos.y),
        toX11Dimension(rect.w),
        toX11Dimension(rect.h),
        0,
        CopyFromParent,
        InputOutput,
        CopyFromParent,
        valueMask,
        &attrs);
    if(child == 0){
        return 0;
    }
    XMapWindow(dpy_, child);
    XFlush(dpy_);
    ownedChildren_.insert(child);
    childOrder_.push_back(ChildState{child, 0});
    return child;
}

void X11SurfaceHost::destroyChildWindow(::Window child){
    if(dpy_ == nullptr || child == 0){
        return;
    }
    auto it = ownedChildren_.find(child);
    if(it == ownedChildren_.end()){
        return;
    }
    ownedChildren_.erase(it);
    childOrder_.erase(std::remove_if(childOrder_.begin(), childOrder_.end(),
        [child](const ChildState &s){ return s.window == child; }),
        childOrder_.end());
    XDestroyWindow(dpy_, child);
    XFlush(dpy_);
}

void X11SurfaceHost::reconfigureChildWindow(::Window child,
                                             const Composition::Rect &rect,
                                             int zOrder){
    if(dpy_ == nullptr || child == 0){
        return;
    }
    if(ownedChildren_.find(child) == ownedChildren_.end()){
        return;
    }
    XMoveResizeWindow(dpy_,
        child,
        toX11Coordinate(rect.pos.x),
        toX11Coordinate(rect.pos.y),
        toX11Dimension(rect.w),
        toX11Dimension(rect.h));
    for(auto &s : childOrder_){
        if(s.window == child){
            s.zOrder = zOrder;
            break;
        }
    }
    restackChildren();
    XFlush(dpy_);
}

void X11SurfaceHost::restackChildren(){
    if(dpy_ == nullptr || childOrder_.size() < 2){
        return;
    }
    // Sort by descending zOrder — XRestackWindows takes a window list
    // where the first entry ends up on top. Stable sort keeps creation
    // order as a tiebreaker so equal-z children don't flicker on
    // every reconfigure.
    std::vector<ChildState> sorted = childOrder_;
    std::stable_sort(sorted.begin(), sorted.end(),
        [](const ChildState &a, const ChildState &b){
            return a.zOrder > b.zOrder;
        });
    std::vector<::Window> windows;
    windows.reserve(sorted.size());
    for(const auto &s : sorted){
        windows.push_back(s.window);
    }
    XRestackWindows(dpy_, windows.data(), static_cast<int>(windows.size()));
}
#endif

bool X11SurfaceHost::isRealized() const {
    std::lock_guard<std::mutex> lk(realizeMutex_);
    return realized_;
}

void X11SurfaceHost::runOnRealize(std::function<void()> action){
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
    // Already realized — fire synchronously. Lock released before the
    // call so subscribers can re-enter X11SurfaceHost without deadlock.
    action();
}

void X11SurfaceHost::reconfigureChild(std::uint64_t hostId,
                                      const Composition::Rect &rectPx,
                                      int zOrder){
#if WTK_NATIVE_X11
    auto it = hostChildren_.find(hostId);
    ::Window child = (it != hostChildren_.end()) ? it->second : 0;
    if(child == 0){
        // First reconfigure for this hostId allocates the child Window.
        // Returns 0 if the toplevel is not yet realized — leave the
        // mapping unset so the caller's runOnRealize retry re-allocates.
        child = createChildWindow(rectPx);
        if(child == 0){
            return;
        }
        hostChildren_.emplace(hostId, child);
    }
    reconfigureChildWindow(child, rectPx, zOrder);
#else
    (void)hostId;
    (void)rectPx;
    (void)zOrder;
#endif
}

void X11SurfaceHost::destroyChild(std::uint64_t hostId){
#if WTK_NATIVE_X11
    auto it = hostChildren_.find(hostId);
    if(it == hostChildren_.end()){
        return;
    }
    destroyChildWindow(it->second);
    hostChildren_.erase(it);
#else
    (void)hostId;
#endif
}

}
