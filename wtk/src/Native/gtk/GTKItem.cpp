#include "NativePrivate/gtk/GTKItem.h"
#include "NativePrivate/gtk/GTKWindowing.h"
#include "omegaWTK/Native/NativeEvent.h"

#include <algorithm>
#include <cmath>
#include <iostream>

namespace OmegaWTK::Native::GTK {

namespace {
static Composition::Rect sanitizeRect(const Composition::Rect &candidate,const Composition::Rect &fallback){
    Composition::Rect sane = candidate;
    if(!std::isfinite(sane.pos.x)){
        sane.pos.x = fallback.pos.x;
    }
    if(!std::isfinite(sane.pos.y)){
        sane.pos.y = fallback.pos.y;
    }
    if(!std::isfinite(sane.w) || sane.w <= 0.f){
        sane.w = fallback.w;
    }
    if(!std::isfinite(sane.h) || sane.h <= 0.f){
        sane.h = fallback.h;
    }
    sane.w = std::max(1.f,sane.w);
    sane.h = std::max(1.f,sane.h);
    return sane;
}
}

GTKItem::GTKItem(Composition::Rect rect,Native::ItemType type):
rect(sanitizeRect(rect,Composition::Rect{Composition::Point2D{0.f,0.f},1.f,1.f})){
    // Legacy path — kept only for the cross-platform make_native_item
    // factory signature. Under the virtual view model nobody calls this on
    // GTK: the root NativeItem is constructed via the GdkSurface-binding ctor
    // below, and non-root NativeItems are subsurfaces created by the
    // VisualTree (§2.13a SurfaceHost), never GtkWidgets. There is no
    // GtkDrawingArea to synthesize in the bare-surface GTK 4 model, so this
    // produces a surfaceless stub.
    (void)type;
    std::cerr << "[OmegaWTK][GTK] WARN: legacy GTKItem(Rect, ItemType) "
                 "constructor invoked — the bare-surface GTK 4 model expects "
                 "GTKAppWindow to bind the root item to the toplevel GdkSurface."
              << std::endl;
}

GTKItem::GTKItem(Composition::Rect rect,GdkSurface *toplevel):
surface(toplevel),
surfaceBorrowed(true),
rect(sanitizeRect(rect,Composition::Rect{Composition::Point2D{0.f,0.f},1.f,1.f})){
    // Borrowed bare toplevel GdkSurface — GTKAppWindow owns its lifetime and
    // destroys it. No GskRenderer is bound (it is not a GtkWidget), so the
    // surface is entirely ours to drive with Vulkan.
}

GdkSurface *GTKItem::getSurface(){
    return surface;
}

void GTKItem::emitIfPossible(NativeEventPtr event){
    if(event != nullptr && hasEventEmitter()){
        sendEventToEmitter(event);
    }
}

void GTKItem::resize(const Composition::Rect &newRect) {
    // Track the latest extent. The toplevel surface size is negotiated by
    // GTKAppWindow (compute-size / layout); the swap-chain resize is driven
    // off that, not from a widget size-request.
    rect = sanitizeRect(newRect,rect);
}

void GTKItem::enable() {
    // Root-surface map/present is GTKAppWindow's responsibility; the item only
    // tracks visibility. Child subsurfaces manage their own mapping through the
    // VisualTree.
    isVisible = true;
}

void GTKItem::disable() {
    isVisible = false;
}

void *GTKItem::getBinding() {
    // Runtime dispatch on the live protocol (a §2.13a BOTH binary carries both
    // accessor sets). The binding is the protocol-native present handle: the
    // wl_surface on Wayland, the X11 Window (XID) on X11.
    if(surface == nullptr){
        return nullptr;
    }
    GdkDisplay *display = gdk_surface_get_display(surface);
    switch(detectWindowingBackend(display)){
#if WTK_NATIVE_WAYLAND
    case WindowingBackend::Wayland:
        return reinterpret_cast<void *>(getWaylandSurface());
#endif
#if WTK_NATIVE_X11
    case WindowingBackend::X11:
        return reinterpret_cast<void *>(getX11Window());
#endif
    default:
        return nullptr;
    }
}

void GTKItem::addChildNativeItem(NativeItemPtr nativeItem) {
    // Under the virtual view model the GTK backend never receives a child
    // NativeItem request. Embedded native surfaces are subsurfaces created by
    // the VisualTree via the §2.13a SurfaceHost child registry, not children
    // of this item.
    (void)nativeItem;
}

void GTKItem::removeChildNativeItem(NativeItemPtr nativeItem) {
    (void)nativeItem;
}

void GTKItem::setClippedView(SharedHandle<NativeItem> clippedView) {
    (void)clippedView;
}

void GTKItem::toggleHorizontalScrollBar(bool &state) {
    // No native scroll container in the bare-surface model — scrolling is
    // virtual (WidgetTreeHost). Report disabled.
    state = false;
}

void GTKItem::toggleVerticalScrollBar(bool &state) {
    state = false;
}

// Independent #ifs (not #if/#elif): in the §2.13a BOTH co-build both accessor
// sets are compiled, and the GDK_IS_*_SURFACE / GDK_IS_*_DISPLAY guards inside
// each make them safe to call on a handle of the other protocol (they return
// null rather than misinterpreting it).
#if WTK_NATIVE_WAYLAND

wl_surface * GTKItem::getWaylandSurface() {
    if(surface == nullptr || !GDK_IS_WAYLAND_SURFACE(surface)){
        return nullptr;
    }
    return gdk_wayland_surface_get_wl_surface(GDK_WAYLAND_SURFACE(surface));
}

wl_display *GTKItem::getWaylandDisplay() {
    if(surface == nullptr){
        return nullptr;
    }
    GdkDisplay *display = gdk_surface_get_display(surface);
    if(display == nullptr || !GDK_IS_WAYLAND_DISPLAY(display)){
        return nullptr;
    }
    return gdk_wayland_display_get_wl_display(GDK_WAYLAND_DISPLAY(display));
}

#endif
#if WTK_NATIVE_X11

// GTK 4.18+ deprecates the whole GdkX11 API (X11-backend sunset); these remain
// the only way to reach the Xlib Display / XID and still work.
Display *GTKItem::getX11Display() {
    if(surface == nullptr){
        return nullptr;
    }
    GdkDisplay *display = gdk_surface_get_display(surface);
    if(display == nullptr || !GDK_IS_X11_DISPLAY(display)){
        return nullptr;
    }
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    return gdk_x11_display_get_xdisplay(display);
G_GNUC_END_IGNORE_DEPRECATIONS
}

Window GTKItem::getX11Window() {
    if(surface == nullptr || !GDK_IS_X11_SURFACE(surface)){
        return 0;
    }
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    return gdk_x11_surface_get_xid(surface);
G_GNUC_END_IGNORE_DEPRECATIONS
}

#endif

GTKItem::~GTKItem(){
    // The surface is borrowed from GTKAppWindow, which destroys it; nothing to
    // release here.
    surface = nullptr;
}

}

namespace OmegaWTK::Native {
NativeItemPtr make_native_item(Composition::Rect rect,Native::ItemType type,NativeItemPtr parent){
    // Cross-platform factory entry point. Under the virtual view model GTK only
    // ever receives the root request from GTKAppWindow, which uses the
    // GdkSurface-binding ctor directly rather than going through this factory.
    // Anything that lands here is a vestige of the old per-view NativeItem
    // model; honour the legacy ctor so dynamic_pointer_cast<GTKItem> still works.
    (void)parent;
    return NativeItemPtr(new GTK::GTKItem(rect,type));
}
}
