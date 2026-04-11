#include "NativePrivate/gtk/GTKItem.h"
#include "omegaWTK/Native/NativeEvent.h"

#include <algorithm>
#include <cmath>

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

static ModifierFlags modifierFlagsFromState(guint state){
    ModifierFlags flags;
    flags.shift = (state & GDK_SHIFT_MASK) != 0;
    flags.control = (state & GDK_CONTROL_MASK) != 0;
    flags.alt = (state & GDK_MOD1_MASK) != 0;
#ifdef GDK_META_MASK
    flags.meta = (state & GDK_META_MASK) != 0;
#endif
#ifdef GDK_SUPER_MASK
    flags.meta = flags.meta || ((state & GDK_SUPER_MASK) != 0);
#endif
    flags.capsLock = (state & GDK_LOCK_MASK) != 0;
    return flags;
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

static gboolean onButtonPressEvent(GtkWidget *,GdkEventButton *event,gpointer data){
    auto *self = static_cast<GTKItem *>(data);
    if(self == nullptr || event == nullptr){
        return FALSE;
    }

    NativeEvent::EventType type = NativeEvent::Unknown;
    if(event->button == 1u){
        type = NativeEvent::LMouseDown;
    }
    else if(event->button == 3u){
        type = NativeEvent::RMouseDown;
    }
    else {
        return FALSE;
    }

    Composition::Point2D clientPos {
        static_cast<float>(event->x),
        static_cast<float>(event->y)
    };
    Composition::Point2D screenPos {
        static_cast<float>(event->x_root),
        static_cast<float>(event->y_root)
    };

    NativeEventParams params = nullptr;
    if(type == NativeEvent::LMouseDown){
        auto *mouseParams = new LMouseDownParams();
        mouseParams->position = clientPos;
        mouseParams->screenPosition = screenPos;
        mouseParams->modifiers = modifierFlagsFromState(event->state);
        mouseParams->clickCount = 1u;
        params = mouseParams;
    }
    else {
        auto *mouseParams = new RMouseDownParams();
        mouseParams->position = clientPos;
        mouseParams->screenPosition = screenPos;
        mouseParams->modifiers = modifierFlagsFromState(event->state);
        mouseParams->clickCount = 1u;
        params = mouseParams;
    }

    self->emitIfPossible(NativeEventPtr(new NativeEvent(type,params)));
    return FALSE;
}

static gboolean onButtonReleaseEvent(GtkWidget *,GdkEventButton *event,gpointer data){
    auto *self = static_cast<GTKItem *>(data);
    if(self == nullptr || event == nullptr){
        return FALSE;
    }

    NativeEvent::EventType type = NativeEvent::Unknown;
    if(event->button == 1u){
        type = NativeEvent::LMouseUp;
    }
    else if(event->button == 3u){
        type = NativeEvent::RMouseUp;
    }
    else {
        return FALSE;
    }

    Composition::Point2D clientPos {
        static_cast<float>(event->x),
        static_cast<float>(event->y)
    };
    Composition::Point2D screenPos {
        static_cast<float>(event->x_root),
        static_cast<float>(event->y_root)
    };

    NativeEventParams params = nullptr;
    if(type == NativeEvent::LMouseUp){
        auto *mouseParams = new LMouseUpParams();
        mouseParams->position = clientPos;
        mouseParams->screenPosition = screenPos;
        mouseParams->modifiers = modifierFlagsFromState(event->state);
        mouseParams->clickCount = 1u;
        params = mouseParams;
    }
    else {
        auto *mouseParams = new RMouseUpParams();
        mouseParams->position = clientPos;
        mouseParams->screenPosition = screenPos;
        mouseParams->modifiers = modifierFlagsFromState(event->state);
        mouseParams->clickCount = 1u;
        params = mouseParams;
    }

    self->emitIfPossible(NativeEventPtr(new NativeEvent(type,params)));
    return FALSE;
}

static gboolean onMotionNotifyEvent(GtkWidget *,GdkEventMotion *event,gpointer data){
    auto *self = static_cast<GTKItem *>(data);
    if(self == nullptr || event == nullptr){
        return FALSE;
    }

    auto *params = new CursorMoveParams();
    params->position = Composition::Point2D {
        static_cast<float>(event->x),
        static_cast<float>(event->y)
    };
    params->screenPosition = Composition::Point2D {
        static_cast<float>(event->x_root),
        static_cast<float>(event->y_root)
    };
    params->modifiers = modifierFlagsFromState(event->state);
    self->emitIfPossible(NativeEventPtr(new NativeEvent(NativeEvent::CursorMove,params)));
    return FALSE;
}

static gboolean onEnterNotifyEvent(GtkWidget *,GdkEventCrossing *event,gpointer data){
    auto *self = static_cast<GTKItem *>(data);
    if(self == nullptr || event == nullptr){
        return FALSE;
    }

    auto *params = new CursorEnterParams();
    params->position = Composition::Point2D {
        static_cast<float>(event->x),
        static_cast<float>(event->y)
    };
    self->emitIfPossible(NativeEventPtr(new NativeEvent(NativeEvent::CursorEnter,params)));
    return FALSE;
}

static gboolean onLeaveNotifyEvent(GtkWidget *,GdkEventCrossing *event,gpointer data){
    auto *self = static_cast<GTKItem *>(data);
    if(self == nullptr || event == nullptr){
        return FALSE;
    }

    auto *params = new CursorExitParams();
    params->position = Composition::Point2D {
        static_cast<float>(event->x),
        static_cast<float>(event->y)
    };
    self->emitIfPossible(NativeEventPtr(new NativeEvent(NativeEvent::CursorExit,params)));
    return FALSE;
}

static gboolean onScrollEvent(GtkWidget *,GdkEventScroll *event,gpointer data){
    auto *self = static_cast<GTKItem *>(data);
    if(self == nullptr || event == nullptr){
        return FALSE;
    }

    Composition::Point2D scrollPos {
        static_cast<float>(event->x),
        static_cast<float>(event->y)
    };

    constexpr double epsilon = 0.0001;
    if(event->direction == GDK_SCROLL_SMOOTH){
        double deltaX = 0.0;
        double deltaY = 0.0;
        if(gdk_event_get_scroll_deltas(reinterpret_cast<GdkEvent *>(event),&deltaX,&deltaY)){
            if(std::fabs(deltaX) > epsilon || std::fabs(deltaY) > epsilon){
                self->emitIfPossible(NativeEventPtr(new NativeEvent(
                        NativeEvent::ScrollWheel,
                        new ScrollParams {static_cast<float>(deltaX),
                                          static_cast<float>(deltaY),
                                          scrollPos})));
            }
            if(std::fabs(deltaX) > epsilon){
                auto type = deltaX > 0.0 ? NativeEvent::ScrollRight : NativeEvent::ScrollLeft;
                self->emitIfPossible(NativeEventPtr(new NativeEvent(type,new ScrollParams {
                    static_cast<float>(deltaX),
                    0.f,
                    scrollPos
                })));
            }
            if(std::fabs(deltaY) > epsilon){
                auto type = deltaY > 0.0 ? NativeEvent::ScrollDown : NativeEvent::ScrollUp;
                self->emitIfPossible(NativeEventPtr(new NativeEvent(type,new ScrollParams {
                    0.f,
                    static_cast<float>(deltaY),
                    scrollPos
                })));
            }
        }
        return FALSE;
    }

    float dirDx = 0.f, dirDy = 0.f;
    switch(event->direction){
        case GDK_SCROLL_UP:
            dirDy = -1.f;
            self->emitIfPossible(NativeEventPtr(new NativeEvent(
                    NativeEvent::ScrollUp,
                    new ScrollParams {0.f,-1.f,scrollPos})));
            break;
        case GDK_SCROLL_DOWN:
            dirDy = 1.f;
            self->emitIfPossible(NativeEventPtr(new NativeEvent(
                    NativeEvent::ScrollDown,
                    new ScrollParams {0.f,1.f,scrollPos})));
            break;
        case GDK_SCROLL_LEFT:
            dirDx = -1.f;
            self->emitIfPossible(NativeEventPtr(new NativeEvent(
                    NativeEvent::ScrollLeft,
                    new ScrollParams {-1.f,0.f,scrollPos})));
            break;
        case GDK_SCROLL_RIGHT:
            dirDx = 1.f;
            self->emitIfPossible(NativeEventPtr(new NativeEvent(
                    NativeEvent::ScrollRight,
                    new ScrollParams {1.f,0.f,scrollPos})));
            break;
        default:
            break;
    }
    if(std::fabs(dirDx) > epsilon || std::fabs(dirDy) > epsilon){
        self->emitIfPossible(NativeEventPtr(new NativeEvent(
                NativeEvent::ScrollWheel,
                new ScrollParams {dirDx * 10.f, dirDy * 10.f, scrollPos})));
    }
    return FALSE;
}

static void onRenderWidgetMap(GtkWidget *widget,gpointer){
    if(widget == nullptr){
        return;
    }
    GdkWindow *window = gtk_widget_get_window(widget);
    if(window != nullptr){
        gdk_window_ensure_native(window);
    }
}

static void onSizeAllocate(GtkWidget *,GtkAllocation *allocation,gpointer data){
    auto *self = static_cast<GTKItem *>(data);
    if(self == nullptr || allocation == nullptr){
        return;
    }
    self->handleAllocation(*allocation);
}

static void onHorizontalAdjustmentChanged(GtkAdjustment *adjustment,gpointer data){
    auto *self = static_cast<GTKItem *>(data);
    if(self == nullptr || adjustment == nullptr){
        return;
    }
    self->handleScrollAdjustmentValue(gtk_adjustment_get_value(adjustment),true);
}

static void onVerticalAdjustmentChanged(GtkAdjustment *adjustment,gpointer data){
    auto *self = static_cast<GTKItem *>(data);
    if(self == nullptr || adjustment == nullptr){
        return;
    }
    self->handleScrollAdjustmentValue(gtk_adjustment_get_value(adjustment),false);
}
}

GTKItem::GTKItem(Composition::Rect rect,Native::ItemType type):
rect(sanitizeRect(rect,Composition::Rect{Composition::Point2D{0.f,0.f},1.f,1.f})),
isScrollItem(type == Native::ScrollItem){
    if(isScrollItem){
        widget = gtk_scrolled_window_new(nullptr,nullptr);
        contentWidget = nullptr;
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(widget),GTK_POLICY_NEVER,GTK_POLICY_NEVER);
        horizontalScrollEnabled = false;
        verticalScrollEnabled = false;
        auto *horizontal = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(widget));
        if(horizontal != nullptr){
            lastHorizontalScrollValue = gtk_adjustment_get_value(horizontal);
            g_signal_connect(horizontal,"value-changed",G_CALLBACK(onHorizontalAdjustmentChanged),this);
        }
        auto *vertical = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(widget));
        if(vertical != nullptr){
            lastVerticalScrollValue = gtk_adjustment_get_value(vertical);
            g_signal_connect(vertical,"value-changed",G_CALLBACK(onVerticalAdjustmentChanged),this);
        }
    }
    else {
        widget = gtk_overlay_new();
        renderWidget = gtk_drawing_area_new();
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        gtk_widget_set_double_buffered(renderWidget,FALSE);
G_GNUC_END_IGNORE_DEPRECATIONS
        gtk_widget_set_app_paintable(renderWidget,TRUE);
        gtk_container_add(GTK_CONTAINER(widget),renderWidget);
        g_signal_connect(renderWidget,"map",G_CALLBACK(onRenderWidgetMap),nullptr);
        gtk_widget_show(renderWidget);
        contentWidget = gtk_fixed_new();
        gtk_widget_set_halign(contentWidget,GTK_ALIGN_FILL);
        gtk_widget_set_valign(contentWidget,GTK_ALIGN_FILL);
        gtk_overlay_add_overlay(GTK_OVERLAY(widget),contentWidget);
        gtk_widget_show(contentWidget);
    }
    if(widget != nullptr){
        g_object_add_weak_pointer(G_OBJECT(widget),reinterpret_cast<gpointer *>(&widget));
        g_signal_connect(widget,"size-allocate",G_CALLBACK(onSizeAllocate),this);
    }
    auto *eventTarget = renderWidget != nullptr ? renderWidget : widget;
    if(eventTarget != nullptr){
        gtk_widget_add_events(eventTarget,
                              GDK_BUTTON_PRESS_MASK
                              | GDK_BUTTON_RELEASE_MASK
                              | GDK_ENTER_NOTIFY_MASK
                              | GDK_LEAVE_NOTIFY_MASK
                              | GDK_POINTER_MOTION_MASK
                              | GDK_SCROLL_MASK
                              | GDK_SMOOTH_SCROLL_MASK);
        g_signal_connect(eventTarget,"button-press-event",G_CALLBACK(onButtonPressEvent),this);
        g_signal_connect(eventTarget,"button-release-event",G_CALLBACK(onButtonReleaseEvent),this);
        g_signal_connect(eventTarget,"motion-notify-event",G_CALLBACK(onMotionNotifyEvent),this);
        g_signal_connect(eventTarget,"enter-notify-event",G_CALLBACK(onEnterNotifyEvent),this);
        g_signal_connect(eventTarget,"leave-notify-event",G_CALLBACK(onLeaveNotifyEvent),this);
        if(!isScrollItem){
            g_signal_connect(eventTarget,"scroll-event",G_CALLBACK(onScrollEvent),this);
        }
    }
    if(renderWidget != nullptr){
        g_object_add_weak_pointer(G_OBJECT(renderWidget),reinterpret_cast<gpointer *>(&renderWidget));
    }
    if(contentWidget != nullptr){
        g_object_add_weak_pointer(G_OBJECT(contentWidget),reinterpret_cast<gpointer *>(&contentWidget));
    }
    updateWidgetSize();
}

GtkWidget *GTKItem::getWidget(){
    return widget;
}

void GTKItem::updateWidgetSize(){
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
    if(widget == nullptr){
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
    setWidgetVisibility(widget,true);
    setWidgetVisibility(renderWidget,true);
    setWidgetVisibility(contentWidget,true);
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
    setWidgetVisibility(widget,false);
    setWidgetVisibility(renderWidget,false);
    setWidgetVisibility(contentWidget,false);
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
    if(container != nullptr && GTK_IS_FIXED(container)){
        auto *existingParent = gtk_widget_get_parent(item->widget);
        if(existingParent != nullptr && GTK_IS_CONTAINER(existingParent)){
            gtk_container_remove(GTK_CONTAINER(existingParent),item->widget);
        }
        gtk_fixed_put(GTK_FIXED(container),item->widget,toGtkCoordinate(item->rect.pos.x),toGtkCoordinate(item->rect.pos.y));
        item->updateWidgetSize();
        childItems.push_back(item);
        const bool childVisible = isVisible && item->isVisible;
        setWidgetVisibility(item->widget,childVisible);
        setWidgetVisibility(item->contentWidget,childVisible);
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
    const bool childVisible = isVisible && next->isVisible;
    setWidgetVisibility(next->widget,childVisible);
    setWidgetVisibility(next->contentWidget,childVisible);
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
    if(renderWidget != nullptr){
        g_object_remove_weak_pointer(G_OBJECT(renderWidget),reinterpret_cast<gpointer *>(&renderWidget));
    }
    if(widget != nullptr){
        g_object_remove_weak_pointer(G_OBJECT(widget),reinterpret_cast<gpointer *>(&widget));
        gtk_widget_destroy(widget);
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
    auto item = NativeItemPtr(new GTK::GTKItem(rect,type));
    if(parent != nullptr){
        parent->addChildNativeItem(item);
    }
    return item;
}
}
