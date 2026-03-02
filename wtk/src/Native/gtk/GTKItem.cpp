#include "NativePrivate/gtk/GTKItem.h"

#include <algorithm>
#include <cmath>

namespace OmegaWTK::Native::GTK {

namespace {
static Core::Rect sanitizeRect(const Core::Rect &candidate,const Core::Rect &fallback){
    Core::Rect sane = candidate;
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

GTKItem::GTKItem(Core::Rect rect,Native::ItemType type):
rect(sanitizeRect(rect,Core::Rect{Core::Position{0.f,0.f},1.f,1.f})),
isScrollItem(type == Native::ScrollItem){
    if(isScrollItem){
        widget = gtk_scrolled_window_new(nullptr,nullptr);
        contentWidget = nullptr;
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(widget),GTK_POLICY_NEVER,GTK_POLICY_NEVER);
        horizontalScrollEnabled = false;
        verticalScrollEnabled = false;
    }
    else {
        widget = gtk_layout_new(nullptr,nullptr);
        contentWidget = nullptr;
    }
    if(widget != nullptr){
        g_object_add_weak_pointer(G_OBJECT(widget),reinterpret_cast<gpointer *>(&widget));
    }
    updateWidgetSize();
}

GtkWidget *GTKItem::getWidget(){
    return widget;
}

void GTKItem::updateWidgetSize(){
    if(widget != nullptr){
        gtk_widget_set_size_request(widget,(gint)rect.w,(gint)rect.h);
    }
    if(contentWidget != nullptr){
        gtk_widget_set_size_request(contentWidget,(gint)rect.w,(gint)rect.h);
    }
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
    if(widget == nullptr){
        return;
    }
    GtkWidget *parent = gtk_widget_get_parent(widget);
    if(parent != nullptr && GTK_IS_LAYOUT(parent)){
        gtk_layout_move(GTK_LAYOUT(parent),widget,(gint)rect.pos.x,(gint)rect.pos.y);
    }
}

GdkWindow *GTKItem::resolveGdkWindow(){
    if(widget == nullptr){
        return nullptr;
    }
    if(!gtk_widget_get_realized(widget)){
        gtk_widget_realize(widget);
    }
    auto *window = gtk_widget_get_window(widget);
    if(window != nullptr){
        return window;
    }
    if(contentWidget != nullptr && contentWidget != widget){
        if(!gtk_widget_get_realized(contentWidget)){
            gtk_widget_realize(contentWidget);
        }
        return gtk_widget_get_window(contentWidget);
    }
    return nullptr;
}

void GTKItem::resize(const Core::Rect &newRect) {
    rect = sanitizeRect(newRect,rect);
    updateWidgetSize();
    moveInParent();
}

void GTKItem::enable() {
    isVisible = true;
    if(widget != nullptr){
        gtk_widget_show(widget);
    }
    if(contentWidget != nullptr){
        gtk_widget_show(contentWidget);
    }
    if(clippedView != nullptr && clippedView->widget != nullptr){
        if(clippedView->isVisible){
            gtk_widget_show(clippedView->widget);
        }
        else {
            gtk_widget_hide(clippedView->widget);
        }
    }
    for(auto &child : childItems){
        if(child == nullptr || child->widget == nullptr){
            continue;
        }
        if(child->isVisible){
            gtk_widget_show(child->widget);
        }
        else {
            gtk_widget_hide(child->widget);
        }
    }
}

void GTKItem::disable() {
    isVisible = false;
    if(widget != nullptr){
        gtk_widget_hide(widget);
    }
    if(contentWidget != nullptr){
        gtk_widget_hide(contentWidget);
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
    auto item = std::dynamic_pointer_cast<GTKItem>(nativeItem);
    if(item == nullptr || item->widget == nullptr){
        return;
    }
    if(isScrollItem){
        setClippedView(nativeItem);
        return;
    }
    auto *container = contentWidget != nullptr ? contentWidget : widget;
    if(container != nullptr && GTK_IS_LAYOUT(container)){
        auto *existingParent = gtk_widget_get_parent(item->widget);
        if(existingParent != nullptr && GTK_IS_CONTAINER(existingParent)){
            gtk_container_remove(GTK_CONTAINER(existingParent),item->widget);
        }
        gtk_layout_put(GTK_LAYOUT(container),item->widget,(gint)item->rect.pos.x,(gint)item->rect.pos.y);
        item->updateWidgetSize();
        childItems.push_back(item);
        if(isVisible && item->isVisible){
            gtk_widget_show(item->widget);
        }
        else {
            gtk_widget_hide(item->widget);
        }
    }
}

void GTKItem::removeChildNativeItem(NativeItemPtr nativeItem) {
    auto item = std::dynamic_pointer_cast<GTKItem>(nativeItem);
    if(item == nullptr || item->widget == nullptr){
        return;
    }
    auto it = childItems.begin();
    while(it != childItems.end()){
        if(*it == item){
            childItems.erase(it);
            break;
        }
        ++it;
    }
    auto *parent = gtk_widget_get_parent(item->widget);
    if(parent != nullptr && GTK_IS_CONTAINER(parent)){
        gtk_container_remove(GTK_CONTAINER(parent),item->widget);
    }
    if(clippedView == item){
        clippedView = nullptr;
    }
}

void GTKItem::setClippedView(SharedHandle<NativeItem> clippedView) {
    if(!isScrollItem || widget == nullptr){
        return;
    }
    auto next = std::dynamic_pointer_cast<GTKItem>(clippedView);
    if(next == nullptr || next->widget == nullptr){
        return;
    }
    if(this->clippedView != nullptr && this->clippedView->widget != nullptr){
        auto *existingParent = gtk_widget_get_parent(this->clippedView->widget);
        if(existingParent != nullptr && GTK_IS_CONTAINER(existingParent)){
            gtk_container_remove(GTK_CONTAINER(existingParent),this->clippedView->widget);
        }
    }
    auto *nextParent = gtk_widget_get_parent(next->widget);
    if(nextParent != nullptr && GTK_IS_CONTAINER(nextParent)){
        gtk_container_remove(GTK_CONTAINER(nextParent),next->widget);
    }
    gtk_container_add(GTK_CONTAINER(widget),next->widget);
    next->updateWidgetSize();
    if(isVisible && next->isVisible){
        gtk_widget_show(next->widget);
    }
    else {
        gtk_widget_hide(next->widget);
    }
    this->clippedView = next;
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
    if(!GDK_IS_X11_WINDOW(window)){
        return 0;
    }
    return gdk_x11_window_get_xid(window);
}

#endif

GTKItem::~GTKItem(){
    if(widget != nullptr){
        g_object_remove_weak_pointer(G_OBJECT(widget),reinterpret_cast<gpointer *>(&widget));
        gtk_widget_destroy(widget);
        widget = nullptr;
    }
    contentWidget = nullptr;
    clippedView = nullptr;
    childItems.clear();
}

}

namespace OmegaWTK::Native {
NativeItemPtr make_native_item(Core::Rect rect,Native::ItemType type,NativeItemPtr parent){
    auto item = NativeItemPtr(new GTK::GTKItem(rect,type));
    if(parent != nullptr){
        parent->addChildNativeItem(item);
    }
    return item;
}
}
