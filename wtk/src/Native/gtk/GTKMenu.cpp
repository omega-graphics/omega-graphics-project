#include "GTKMenu.h"

#include <algorithm>
#include <iostream>
#include <memory>
#include <unordered_map>

#include <gtk/gtk.h>

namespace OmegaWTK::Native::GTK {

    namespace {
        guint shortcutToKeyval(const NativeMenuShortcut & sc) {
            if(sc.key.empty()) return 0;
            const OmegaCommon::String & k = sc.key;
            if(k.size() > 1) {
                if(k == "F1")  return GDK_KEY_F1;
                if(k == "F2")  return GDK_KEY_F2;
                if(k == "F3")  return GDK_KEY_F3;
                if(k == "F4")  return GDK_KEY_F4;
                if(k == "F5")  return GDK_KEY_F5;
                if(k == "F6")  return GDK_KEY_F6;
                if(k == "F7")  return GDK_KEY_F7;
                if(k == "F8")  return GDK_KEY_F8;
                if(k == "F9")  return GDK_KEY_F9;
                if(k == "F10") return GDK_KEY_F10;
                if(k == "F11") return GDK_KEY_F11;
                if(k == "F12") return GDK_KEY_F12;
                if(k == "Tab")       return GDK_KEY_Tab;
                if(k == "Space")     return GDK_KEY_space;
                if(k == "Return" || k == "Enter") return GDK_KEY_Return;
                if(k == "Backspace") return GDK_KEY_BackSpace;
                if(k == "Delete")    return GDK_KEY_Delete;
                if(k == "Escape")    return GDK_KEY_Escape;
                if(k == "Up")        return GDK_KEY_Up;
                if(k == "Down")      return GDK_KEY_Down;
                if(k == "Left")      return GDK_KEY_Left;
                if(k == "Right")     return GDK_KEY_Right;
                if(k == "Home")      return GDK_KEY_Home;
                if(k == "End")       return GDK_KEY_End;
                if(k == "PageUp")    return GDK_KEY_Page_Up;
                if(k == "PageDown")  return GDK_KEY_Page_Down;
                return 0;
            }
            char c = k[0];
            if(c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            return gdk_unicode_to_keyval((guint)c);
        }

        GdkModifierType shortcutToMods(const NativeMenuShortcut & sc) {
            guint m = 0;
            if(sc.meta)  m |= GDK_CONTROL_MASK;
            if(sc.shift) m |= GDK_SHIFT_MASK;
            if(sc.alt)   m |= GDK_MOD1_MASK;
            return (GdkModifierType)m;
        }
    }

    class GTKMenu;

    class GTKMenuItem : public NativeMenuItem {
        GtkMenuItem *ptr = nullptr;
        std::weak_ptr<GTKMenu> parent;
        friend class GTKMenu;
        bool hasSubMenu = false;
        SharedHandle<GTKMenu> subMenu = nullptr;
        gulong activateHandlerId = 0;
        NativeMenuItemType type = NativeMenuItemType::Button;
        bool checkedState = false;
        guint accelKey = 0;
        GdkModifierType accelMods = (GdkModifierType)0;
        bool suppressActivate = false;  // Used to bypass our handler when toggling state programmatically.
        static void selectThisItem(GtkMenuItem *item,gpointer data);

        void applyAccelToWidget();
    public:
        void setState(bool state) override {
            if(ptr == nullptr) return;
            gtk_widget_set_sensitive(GTK_WIDGET(ptr), state ? TRUE : FALSE);
        };
        void setTitle(const OmegaCommon::String & title) override {
            if(ptr == nullptr) return;
            gtk_menu_item_set_label(ptr, title.c_str());
        }
        void setChecked(bool checked) override {
            if(ptr == nullptr) return;
            if(type != NativeMenuItemType::Checkbox && type != NativeMenuItemType::Radio) return;
            checkedState = checked;
            if(GTK_IS_CHECK_MENU_ITEM(ptr)) {
                suppressActivate = true;
                gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(ptr), checked ? TRUE : FALSE);
                suppressActivate = false;
            }
        }
        bool isChecked() const override {
            if(ptr && GTK_IS_CHECK_MENU_ITEM(ptr)) {
                return gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(ptr)) == TRUE;
            }
            return checkedState;
        }
        void setShortcut(const NativeMenuShortcut & sc) override {
            accelKey = shortcutToKeyval(sc);
            accelMods = shortcutToMods(sc);
            applyAccelToWidget();
        }
        NativeMenuItemType getType() const override { return type; }

        /// Construct a regular menu item from a label, parent menu, and an optional sub menu.
        GTKMenuItem(const OmegaCommon::String & str, SharedHandle<GTKMenu> parent, bool hasSubMenu, SharedHandle<GTKMenu> subMenu);
        /// Construct a separator.
        GTKMenuItem(){
            ptr = GTK_MENU_ITEM(gtk_separator_menu_item_new());
            type = NativeMenuItemType::Separator;
            if(ptr != nullptr){
                g_object_ref_sink(ptr);
                gtk_widget_set_sensitive(GTK_WIDGET(ptr), FALSE);
            }
        };
        /// Construct a checkbox or radio item.
        GTKMenuItem(const OmegaCommon::String & str, NativeMenuItemType t, bool initialChecked);
        ~GTKMenuItem();
    };

    class GTKMenu : public NativeMenu, public std::enable_shared_from_this<GTKMenu> {
        GtkMenuShell *ptr = nullptr;
        OmegaCommon::Vector<SharedHandle<GTKMenuItem>> items;
        GtkAccelGroup *accelGroup = nullptr;
        gulong popupShownHandlerId = 0;
        friend class GTKMenuItem;

        void onSelectItem(GTKMenuItem *item){
            unsigned idx = 0;
            auto it = items.begin();
            while(it != items.end()){
                if(it->get() == item) break;
                ++it; ++idx;
            }
            if(idx == items.size()){
                std::cout << "Menu Item Not added to Menu... Will not select" << std::endl;
                return;
            }
            if(hasDelegate && delegate != nullptr){
                delegate->onSelectItem(idx);
            }
        };

        static void onPopupShown(GtkWidget *, gpointer data) {
            auto *self = static_cast<GTKMenu *>(data);
            if(self) self->runValidation();
        }

    public:
        GTKMenu(const OmegaCommon::String & name){
            (void)name;
            ptr = GTK_MENU_SHELL(gtk_menu_new());
            if(ptr != nullptr){
                g_object_ref_sink(ptr);
                accelGroup = gtk_accel_group_new();
                gtk_menu_set_accel_group(GTK_MENU(ptr), accelGroup);
                popupShownHandlerId = g_signal_connect(ptr, "show",
                                                       G_CALLBACK(&GTKMenu::onPopupShown),
                                                       this);
            }
        };

        GtkAccelGroup *getAccelGroup() const { return accelGroup; }
        GtkMenuShell *getMenuShell() const { return ptr; }

        void runValidation() {
            if(!hasDelegate || delegate == nullptr) return;
            for(unsigned i = 0; i < items.size(); ++i) {
                bool enabled = delegate->onValidateItem(i);
                if(items[i]->ptr) {
                    gtk_widget_set_sensitive(GTK_WIDGET(items[i]->ptr), enabled ? TRUE : FALSE);
                }
            }
        }

        void addMenuItem(NMI menu_item) override{
            auto item = std::dynamic_pointer_cast<GTKMenuItem>(menu_item);
            if(item == nullptr || item->ptr == nullptr || ptr == nullptr) return;
            auto existing = std::find(items.begin(), items.end(), item);
            if(existing != items.end()) return;
            auto *currentParent = gtk_widget_get_parent(GTK_WIDGET(item->ptr));
            if(currentParent != nullptr && GTK_IS_CONTAINER(currentParent)){
                gtk_container_remove(GTK_CONTAINER(currentParent), GTK_WIDGET(item->ptr));
            }
            item->parent = weak_from_this();
            gtk_menu_shell_append(ptr, GTK_WIDGET(item->ptr));
            items.push_back(item);
            item->applyAccelToWidget();
            gtk_widget_show(GTK_WIDGET(item->ptr));
        };
        void insertMenuItem(NMI menu_item, unsigned int idx) override{
            auto item = std::dynamic_pointer_cast<GTKMenuItem>(menu_item);
            if(item == nullptr || item->ptr == nullptr || ptr == nullptr) return;
            auto existing = std::find(items.begin(), items.end(), item);
            if(existing != items.end()) return;
            auto insertIdx = static_cast<unsigned>(std::min<size_t>(idx, items.size()));
            auto *currentParent = gtk_widget_get_parent(GTK_WIDGET(item->ptr));
            if(currentParent != nullptr && GTK_IS_CONTAINER(currentParent)){
                gtk_container_remove(GTK_CONTAINER(currentParent), GTK_WIDGET(item->ptr));
            }
            item->parent = weak_from_this();
            gtk_menu_shell_insert(ptr, GTK_WIDGET(item->ptr), static_cast<gint>(insertIdx));
            items.insert(items.begin() + insertIdx, item);
            item->applyAccelToWidget();
            gtk_widget_show(GTK_WIDGET(item->ptr));
        };
        void removeMenuItem(unsigned idx) override {
            if(idx >= items.size()) return;
            auto item = items[idx];
            if(item && item->ptr) {
                gtk_container_remove(GTK_CONTAINER(ptr), GTK_WIDGET(item->ptr));
            }
            items.erase(items.begin() + idx);
        }
        void removeAllItems() override {
            for(auto & it : items) {
                if(it && it->ptr) {
                    gtk_container_remove(GTK_CONTAINER(ptr), GTK_WIDGET(it->ptr));
                }
            }
            items.clear();
        }
        unsigned itemCount() const override { return (unsigned)items.size(); }
        void * getNativeBinding() override{
            return (void *)ptr;
        };
        ~GTKMenu(){
            items.clear();
            if(ptr != nullptr){
                if(popupShownHandlerId != 0) {
                    g_signal_handler_disconnect(ptr, popupShownHandlerId);
                    popupShownHandlerId = 0;
                }
                g_object_unref(ptr);
                ptr = nullptr;
            }
            if(accelGroup != nullptr) {
                g_object_unref(accelGroup);
                accelGroup = nullptr;
            }
        };
    };

    void GTKMenuItem::applyAccelToWidget() {
        if(ptr == nullptr || accelKey == 0) return;
        auto p = parent.lock();
        if(!p || !p->getAccelGroup()) return;
        // Add the accelerator and let GTK draw the shortcut hint on the item.
        gtk_widget_add_accelerator(GTK_WIDGET(ptr), "activate",
                                   p->getAccelGroup(),
                                   accelKey, accelMods,
                                   GTK_ACCEL_VISIBLE);
    }

    GTKMenuItem::GTKMenuItem(const OmegaCommon::String & str,
                             SharedHandle<GTKMenu> parent_,
                             bool hasSubMenu_,
                             SharedHandle<GTKMenu> subMenu_){
        ptr = GTK_MENU_ITEM(gtk_menu_item_new_with_label(str.c_str()));
        type = NativeMenuItemType::Button;
        if(ptr != nullptr){
            g_object_ref_sink(ptr);
        }
        this->parent = parent_;

        if(ptr != nullptr &&
           hasSubMenu_ &&
           subMenu_ != nullptr &&
           subMenu_ != parent_ &&
           subMenu_->getMenuShell() != nullptr &&
           GTK_IS_MENU(subMenu_->getMenuShell())){
            this->hasSubMenu = true;
            this->subMenu = subMenu_;
            gtk_menu_item_set_submenu(ptr, GTK_WIDGET(this->subMenu->getMenuShell()));
        } else {
            this->hasSubMenu = false;
            this->subMenu = nullptr;
        }

        if(ptr != nullptr){
            activateHandlerId = g_signal_connect(ptr, "activate", G_CALLBACK(selectThisItem), (gpointer)this);
        }
    }

    GTKMenuItem::GTKMenuItem(const OmegaCommon::String & str,
                             NativeMenuItemType t,
                             bool initialChecked) {
        type = t;
        checkedState = initialChecked;
        if(t == NativeMenuItemType::Radio) {
            // Use a check menu item drawn as a radio button. Group-exclusive
            // semantics across multiple items are the caller's responsibility.
            GtkWidget *w = gtk_check_menu_item_new_with_label(str.c_str());
            gtk_check_menu_item_set_draw_as_radio(GTK_CHECK_MENU_ITEM(w), TRUE);
            ptr = GTK_MENU_ITEM(w);
        } else {
            ptr = GTK_MENU_ITEM(gtk_check_menu_item_new_with_label(str.c_str()));
        }
        if(ptr != nullptr) {
            g_object_ref_sink(ptr);
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(ptr), initialChecked ? TRUE : FALSE);
            activateHandlerId = g_signal_connect(ptr, "activate", G_CALLBACK(selectThisItem), (gpointer)this);
        }
    }

    void GTKMenuItem::selectThisItem(GtkMenuItem *item, gpointer data){
        (void)item;
        auto *self = static_cast<GTKMenuItem *>(data);
        if(self == nullptr || self->suppressActivate) return;
        if(self->type == NativeMenuItemType::Checkbox || self->type == NativeMenuItemType::Radio) {
            // Mirror the GTK check state into our cached state so isChecked()
            // is consistent for callers that don't query the widget directly.
            self->checkedState = gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(self->ptr)) == TRUE;
        }
        auto parent = self->parent.lock();
        if(parent == nullptr) return;
        parent->onSelectItem(self);
    }

    GTKMenuItem::~GTKMenuItem(){
        if(ptr != nullptr){
            if(activateHandlerId != 0){
                g_signal_handler_disconnect(ptr, activateHandlerId);
                activateHandlerId = 0;
            }
            g_object_unref(ptr);
            ptr = nullptr;
        }
        subMenu = nullptr;
        parent.reset();
    }

    NM make_gtk_menu(const OmegaCommon::String &name){
        return std::make_shared<GTKMenu>(name);
    }
    NMI make_gtk_menu_item(const OmegaCommon::String &str, NM parent, bool hasSubMenu, NM subMenu){
        return std::make_shared<GTKMenuItem>(str,
                                             std::dynamic_pointer_cast<GTKMenu>(parent),
                                             hasSubMenu,
                                             std::dynamic_pointer_cast<GTKMenu>(subMenu));
    }
    NMI make_gtk_menu_seperator(){
        return std::make_shared<GTKMenuItem>();
    }
    NMI make_gtk_checkbox_item(const OmegaCommon::String & str, NM parent, bool initialChecked){
        (void)parent;
        return std::make_shared<GTKMenuItem>(str, NativeMenuItemType::Checkbox, initialChecked);
    }
    NMI make_gtk_radio_item(const OmegaCommon::String & str, NM parent, bool initialChecked){
        (void)parent;
        return std::make_shared<GTKMenuItem>(str, NativeMenuItemType::Radio, initialChecked);
    }
    void show_gtk_context_menu(NM menu, Composition::Point2D screenPos){
        auto gm = std::dynamic_pointer_cast<GTKMenu>(menu);
        if(!gm || !gm->getMenuShell()) return;
        // runValidation runs via the "show" signal handler.
        // We pop up at the pointer; screenPos is currently advisory because GTK3
        // chooses a position based on the triggering event when one is available.
        (void)screenPos;
        gtk_menu_popup_at_pointer(GTK_MENU(gm->getMenuShell()), nullptr);
    }
}
