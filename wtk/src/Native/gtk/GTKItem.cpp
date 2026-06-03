#include "NativePrivate/gtk/GTKItem.h"
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

static gint toGtkCoordinate(float value){
    if(!std::isfinite(value)){
        return 0;
    }
    return static_cast<gint>(std::lround(value));
}

static gint toGtkSize(float value){
    if(!std::isfinite(value) || value <= 0.f){
        return 1;
    }
    return std::max<gint>(1,toGtkCoordinate(value));
}

static void setWidgetVisibility(GtkWidget *widget,bool visible){
    if(widget == nullptr){
        return;
    }
    if(visible){
        gtk_widget_show(widget);
    }
    else {
        gtk_widget_hide(widget);
    }
}
}

GTKItem::GTKItem(Composition::Rect rect,Native::ItemType type):
rect(sanitizeRect(rect,Composition::Rect{Composition::Point2D{0.f,0.f},1.f,1.f})),
isScrollItem(type == Native::ScrollItem){
    // Legacy path — only kept for the cross-platform make_native_item
    // factory signature. Under §2.13 / the virtual view model nobody
    // calls this on GTK: the root NativeItem is constructed via the
    // toplevel-binding ctor below, and non-root NativeItems are not
    // requested by the virtual view tree. If something does reach this
    // path, log and synthesize a minimal non-rendering widget so the
    // call doesn't crash.
    std::cerr << "[OmegaWTK][GTK] WARN: legacy GTKItem(Rect, ItemType) "
                 "constructor invoked — virtual view model expects "
                 "GTKAppWindow to bind the root item to the toplevel."
              << std::endl;
    if(isScrollItem){
        widget = gtk_scrolled_window_new(nullptr,nullptr);
        contentWidget = nullptr;
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(widget),GTK_POLICY_NEVER,GTK_POLICY_NEVER);
    }
    else {
        widget = gtk_drawing_area_new();
        renderWidget = widget;
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        gtk_widget_set_double_buffered(widget,FALSE);
G_GNUC_END_IGNORE_DEPRECATIONS
        gtk_widget_set_app_paintable(widget,TRUE);
        contentWidget = nullptr;
    }
    if(widget != nullptr){
        g_object_add_weak_pointer(G_OBJECT(widget),reinterpret_cast<gpointer *>(&widget));
    }
    if(renderWidget != nullptr){
        g_object_add_weak_pointer(G_OBJECT(renderWidget),reinterpret_cast<gpointer *>(&renderWidget));
    }
    if(contentWidget != nullptr){
        g_object_add_weak_pointer(G_OBJECT(contentWidget),reinterpret_cast<gpointer *>(&contentWidget));
    }
    updateWidgetSize();
}

GTKItem::GTKItem(Composition::Rect rect,GtkWidget *toplevel):
widget(toplevel),
renderWidget(nullptr),
widgetBorrowed(true),
rect(sanitizeRect(rect,Composition::Rect{Composition::Point2D{0.f,0.f},1.f,1.f})){
    // Borrowed toplevel — GTKAppWindow owns lifetime. We weak-pointer
    // the widget so a delete-event tear-down by GTK doesn't leave us
    // holding a dangling reference; the dtor's `widgetBorrowed` flag
    // keeps us from calling gtk_widget_destroy on something we don't
    // own. We deliberately leave `renderWidget` null: a single
    // `g_object_add_weak_pointer` clears exactly one pointer slot, so
    // storing the same toplevel in both `widget` and `renderWidget`
    // would leave `renderWidget` dangling after a delete-event. Code
    // that wants the render target reads it through `resolveGdkWindow`,
    // which falls back to `widget` when `renderWidget` is null.
    if(widget != nullptr){
        g_object_add_weak_pointer(G_OBJECT(widget),reinterpret_cast<gpointer *>(&widget));
    }
}

GtkWidget *GTKItem::getWidget(){
    return widget;
}

void GTKItem::updateWidgetSize(){
    if(widgetBorrowed){
        // Toplevel sizing is GTKAppWindow's job (gtk_window_resize). Do
        // not force a size-request on the borrowed widget — that would
        // make the toplevel un-shrinkable below its current size.
        return;
    }
    if(widget != nullptr){
        gtk_widget_set_size_request(widget,toGtkSize(rect.w),toGtkSize(rect.h));
    }
    if(renderWidget != nullptr){
        gtk_widget_set_size_request(renderWidget,toGtkSize(rect.w),toGtkSize(rect.h));
    }
    if(contentWidget != nullptr){
        gtk_widget_set_size_request(contentWidget,toGtkSize(rect.w),toGtkSize(rect.h));
    }
}

void GTKItem::emitIfPossible(NativeEventPtr event){
    if(event != nullptr && hasEventEmitter()){
        sendEventToEmitter(event);
    }
}

void GTKItem::handleAllocation(const GtkAllocation &allocation){
    Composition::Rect allocatedRect {
        rect.pos,
        static_cast<float>(allocation.width),
        static_cast<float>(allocation.height)
    };
    auto sanitized = sanitizeRect(allocatedRect,rect);
    if(rect.w == sanitized.w && rect.h == sanitized.h){
        return;
    }
    rect.w = sanitized.w;
    rect.h = sanitized.h;
    emitIfPossible(NativeEventPtr(new NativeEvent(NativeEvent::ViewResize,new ViewResize {rect})));
}

void GTKItem::handleScrollAdjustmentValue(double value,bool horizontal){
    if(!isScrollItem){
        return;
    }

    const double previous = horizontal ? lastHorizontalScrollValue : lastVerticalScrollValue;
    if(horizontal){
        lastHorizontalScrollValue = value;
    }
    else {
        lastVerticalScrollValue = value;
    }

    const double delta = value - previous;
    constexpr double epsilon = 0.0001;
    if(std::fabs(delta) <= epsilon){
        return;
    }

    Composition::Point2D pos {0.f, 0.f};
    const auto type = horizontal
        ? (delta > 0.0 ? NativeEvent::ScrollRight : NativeEvent::ScrollLeft)
        : (delta > 0.0 ? NativeEvent::ScrollDown : NativeEvent::ScrollUp);
    emitIfPossible(NativeEventPtr(new NativeEvent(type,new ScrollParams {
        horizontal ? static_cast<float>(delta) : 0.f,
        horizontal ? 0.f : static_cast<float>(delta),
        pos
    })));
}

void GTKItem::applyScrollPolicy(){
    if(!isScrollItem || widget == nullptr){
        return;
    }
    auto horizontal = horizontalScrollEnabled ? GTK_POLICY_AUTOMATIC : GTK_POLICY_NEVER;
    auto vertical = verticalScrollEnabled ? GTK_POLICY_AUTOMATIC : GTK_POLICY_NEVER;
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(widget),horizontal,vertical);
}

void GTKItem::moveInParent(){
    if(widget == nullptr || widgetBorrowed){
        return;
    }
    GtkWidget *parent = gtk_widget_get_parent(widget);
    if(parent != nullptr && GTK_IS_FIXED(parent)){
        gtk_fixed_move(GTK_FIXED(parent),widget,toGtkCoordinate(rect.pos.x),toGtkCoordinate(rect.pos.y));
    }
}

GdkWindow *GTKItem::resolveGdkWindow(){
    auto *target = renderWidget != nullptr ? renderWidget : widget;
    if(target == nullptr){
        return nullptr;
    }
    if(!gtk_widget_get_realized(target)){
        return nullptr;
    }
    return gtk_widget_get_window(target);
}

void GTKItem::resize(const Composition::Rect &newRect) {
    rect = sanitizeRect(newRect,rect);
    updateWidgetSize();
    moveInParent();
}

void GTKItem::enable() {
    isVisible = true;
    if(!widgetBorrowed){
        setWidgetVisibility(widget,true);
        setWidgetVisibility(renderWidget,true);
        setWidgetVisibility(contentWidget,true);
    }
    if(clippedView != nullptr && clippedView->widget != nullptr){
        setWidgetVisibility(clippedView->widget,clippedView->isVisible);
        setWidgetVisibility(clippedView->contentWidget,clippedView->isVisible);
    }
    for(auto &child : childItems){
        if(child == nullptr || child->widget == nullptr){
            continue;
        }
        setWidgetVisibility(child->widget,child->isVisible);
        setWidgetVisibility(child->contentWidget,child->isVisible);
    }
}

void GTKItem::disable() {
    isVisible = false;
    if(!widgetBorrowed){
        setWidgetVisibility(widget,false);
        setWidgetVisibility(renderWidget,false);
        setWidgetVisibility(contentWidget,false);
    }
}

void *GTKItem::getBinding() {
#ifdef WTK_NATIVE_WAYLAND
    return reinterpret_cast<void *>(getSurface());
#elif WTK_NATIVE_X11
    return reinterpret_cast<void *>(getX11Window());
#else
    return nullptr;
#endif
}

void GTKItem::addChildNativeItem(NativeItemPtr nativeItem) {
    // Under the virtual view model the GTK backend never receives a
    // child NativeItem request. Embedded native surfaces go through
    // X11SurfaceHost via the visual tree, not through NativeItem.
    (void)nativeItem;
}

void GTKItem::removeChildNativeItem(NativeItemPtr nativeItem) {
    (void)nativeItem;
}

void GTKItem::setClippedView(SharedHandle<NativeItem> clippedView) {
    (void)clippedView;
}

void GTKItem::toggleHorizontalScrollBar(bool &state) {
    if(!isScrollItem || widget == nullptr){
        return;
    }
    horizontalScrollEnabled = state;
    state = horizontalScrollEnabled;
    applyScrollPolicy();
}

void GTKItem::toggleVerticalScrollBar(bool &state) {
    if(!isScrollItem || widget == nullptr){
        return;
    }
    verticalScrollEnabled = state;
    state = verticalScrollEnabled;
    applyScrollPolicy();
}

#if WTK_NATIVE_WAYLAND

wl_surface * GTKItem::getSurface() {
    auto *window = resolveGdkWindow();
    if(window == nullptr){
        return nullptr;
    }
    if(!gdk_window_ensure_native(window)){
        return nullptr;
    }
    if(!GDK_IS_WAYLAND_WINDOW(window)){
        return nullptr;
    }
    return gdk_wayland_window_get_wl_surface(window);
}

wl_display *GTKItem::getDisplay() {
    auto *window = resolveGdkWindow();
    if(window == nullptr){
        return nullptr;
    }
    auto *display = gdk_window_get_display(window);
    if(display == nullptr){
        return nullptr;
    }
    if(!GDK_IS_WAYLAND_DISPLAY(display)){
        return nullptr;
    }
    return gdk_wayland_display_get_wl_display(display);
}

#elif WTK_NATIVE_X11

Display *GTKItem::getDisplay() {
    auto *window = resolveGdkWindow();
    if(window == nullptr){
        return nullptr;
    }
    auto *display = gdk_window_get_display(window);
    if(display == nullptr || !GDK_IS_X11_DISPLAY(display)){
        return nullptr;
    }
    return gdk_x11_display_get_xdisplay(display);
}

Window GTKItem::getX11Window() {
    auto *window = resolveGdkWindow();
    if(window == nullptr){
        return 0;
    }
    if(!gdk_window_ensure_native(window)){
        return 0;
    }
    if(!GDK_IS_X11_WINDOW(window)){
        return 0;
    }
    return gdk_x11_window_get_xid(window);
}

#endif

GTKItem::~GTKItem(){
    if(contentWidget != nullptr){
        g_object_remove_weak_pointer(G_OBJECT(contentWidget),reinterpret_cast<gpointer *>(&contentWidget));
    }
    if(renderWidget != nullptr && (!widgetBorrowed || renderWidget != widget)){
        g_object_remove_weak_pointer(G_OBJECT(renderWidget),reinterpret_cast<gpointer *>(&renderWidget));
    }
    if(widget != nullptr){
        g_object_remove_weak_pointer(G_OBJECT(widget),reinterpret_cast<gpointer *>(&widget));
        if(!widgetBorrowed){
            gtk_widget_destroy(widget);
        }
        widget = nullptr;
    }
    renderWidget = nullptr;
    contentWidget = nullptr;
    clippedView = nullptr;
    childItems.clear();
}

}

namespace OmegaWTK::Native {
NativeItemPtr make_native_item(Composition::Rect rect,Native::ItemType type,NativeItemPtr parent){
    // Cross-platform factory entry point. Under the virtual view model
    // GTK only ever receives the root request from GTKAppWindow, and
    // GTKAppWindow uses the toplevel-binding ctor directly rather than
    // going through this factory. Anything that lands here is a vestige
    // of the old per-view NativeItem model; we honour the legacy ctor
    // so dynamic_pointer_cast<GTKItem> on the result still works.
    (void)parent;
    return NativeItemPtr(new GTK::GTKItem(rect,type));
}
}
