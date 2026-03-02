#include "GTKMenu.h"

#include <algorithm>
#include <iostream>
#include <memory>

#include <gtk/gtk.h>

namespace OmegaWTK::Native::GTK {
    class GTKMenu;

    class GTKMenuItem : public NativeMenuItem {
        GtkMenuItem *ptr = nullptr;
        std::weak_ptr<GTKMenu> parent;
        friend class GTKMenu;
        bool hasSubMenu = false;
        SharedHandle<GTKMenu> subMenu = nullptr;
        gulong activateHandlerId = 0;
        static void selectThisItem(GtkMenuItem *item,gpointer data);
    public:
        void setState(bool state) override {
            if(ptr == nullptr){
                return;
            }
            gtk_widget_set_sensitive(GTK_WIDGET(ptr),state ? TRUE : FALSE);
        };
        /**
          Construct a GTKMenuItem from a label, parent menu, and a optional subMenu
        */
        GTKMenuItem(const OmegaCommon::String &str, SharedHandle<GTKMenu> parent, bool hasSubMenu, SharedHandle<GTKMenu> subMenu);
        /**
            Construct a GTKMenuItem seperator
        */
        GTKMenuItem(){
            ptr = GTK_MENU_ITEM(gtk_separator_menu_item_new());
            if(ptr != nullptr){
                g_object_ref_sink(ptr);
                gtk_widget_set_sensitive(GTK_WIDGET(ptr),FALSE);
            }
        };
        ~GTKMenuItem();
    };

    class GTKMenu : public NativeMenu, public std::enable_shared_from_this<GTKMenu> {
        GtkMenuShell *ptr = nullptr;
        OmegaCommon::Vector<SharedHandle<GTKMenuItem>> items;
        friend class GTKMenuItem;
        void onSelectItem(GTKMenuItem *item){
            auto item_it = items.begin();
            unsigned idx = 0;
            while(item_it != items.end()){
                if(item_it->get() == item){
                    break;
                };
                ++item_it;
                ++idx;
            };
            if(idx == items.size()){
                std::cout << "Menu Item Not added to Menu... Will not select" << std::endl;
                return;
            };

            if(hasDelegate && delegate != nullptr){
                delegate->onSelectItem(idx);
            };

        };
    public:
        GTKMenu(const OmegaCommon::String & name){
            (void)name;
            ptr = GTK_MENU_SHELL(gtk_menu_new());
            if(ptr != nullptr){
                g_object_ref_sink(ptr);
            }
        };
        void addMenuItem(NMI menu_item) override{
            auto item = std::dynamic_pointer_cast<GTKMenuItem>(menu_item);
            if(item == nullptr || item->ptr == nullptr || ptr == nullptr){
                return;
            }
            auto existing = std::find(items.begin(),items.end(),item);
            if(existing != items.end()){
                return;
            }
            auto *currentParent = gtk_widget_get_parent(GTK_WIDGET(item->ptr));
            if(currentParent != nullptr && GTK_IS_CONTAINER(currentParent)){
                gtk_container_remove(GTK_CONTAINER(currentParent),GTK_WIDGET(item->ptr));
            }
            item->parent = weak_from_this();
            gtk_menu_shell_append(ptr,GTK_WIDGET(item->ptr));
            items.push_back(item);
            gtk_widget_show(GTK_WIDGET(item->ptr));
        };
        void insertMenuItem(NMI menu_item, unsigned int idx) override{
            auto item = std::dynamic_pointer_cast<GTKMenuItem>(menu_item);
            if(item == nullptr || item->ptr == nullptr || ptr == nullptr){
                return;
            }
            auto existing = std::find(items.begin(),items.end(),item);
            if(existing != items.end()){
                return;
            }
            auto insertIdx = static_cast<unsigned>(std::min<size_t>(idx,items.size()));
            auto *currentParent = gtk_widget_get_parent(GTK_WIDGET(item->ptr));
            if(currentParent != nullptr && GTK_IS_CONTAINER(currentParent)){
                gtk_container_remove(GTK_CONTAINER(currentParent),GTK_WIDGET(item->ptr));
            }
            item->parent = weak_from_this();
            gtk_menu_shell_insert(ptr,GTK_WIDGET(item->ptr),static_cast<gint>(insertIdx));
            items.insert(items.begin() + insertIdx,item);
            gtk_widget_show(GTK_WIDGET(item->ptr));
        };
        void * getNativeBinding() override{
            return (void *)ptr;
        };
        ~GTKMenu(){
            items.clear();
            if(ptr != nullptr){
                g_object_unref(ptr);
                ptr = nullptr;
            }
        };
    };

    GTKMenuItem::GTKMenuItem(const OmegaCommon::String &str, SharedHandle<GTKMenu> parent, bool hasSubMenu, SharedHandle<GTKMenu> subMenu){
        ptr = GTK_MENU_ITEM(gtk_menu_item_new_with_label(str.c_str()));
        if(ptr != nullptr){
            g_object_ref_sink(ptr);
        }
        this->parent = parent;

        if(ptr != nullptr &&
           hasSubMenu &&
           subMenu != nullptr &&
           subMenu != parent &&
           subMenu->ptr != nullptr &&
           GTK_IS_MENU(subMenu->ptr)){
            this->hasSubMenu = true;
            this->subMenu = subMenu;
            gtk_menu_item_set_submenu(ptr,GTK_WIDGET(this->subMenu->ptr));
        }
        else {
            this->hasSubMenu = false;
            this->subMenu = nullptr;
        }

        if(ptr != nullptr){
            activateHandlerId = g_signal_connect(ptr,"activate",G_CALLBACK(selectThisItem),(gpointer)this);
        }
    };

    void GTKMenuItem::selectThisItem(GtkMenuItem *item,gpointer data){
        (void)item;
        auto *self = static_cast<GTKMenuItem *>(data);
        if(self == nullptr){
            return;
        }
        auto parent = self->parent.lock();
        if(parent == nullptr){
            return;
        }
        parent->onSelectItem(self);
    };

    GTKMenuItem::~GTKMenuItem(){
        if(ptr != nullptr){
            if(activateHandlerId != 0){
                g_signal_handler_disconnect(ptr,activateHandlerId);
                activateHandlerId = 0;
            }
            g_object_unref(ptr);
            ptr = nullptr;
        }
        subMenu = nullptr;
        parent.reset();
    }

    NM make_gtk_menu(const OmegaCommon::String &name){
        return NM(new GTKMenu(name));
    };
    NMI make_gtk_menu_item(const OmegaCommon::String &str, NM parent, bool hasSubMenu, NM subMenu){
        return NMI(new GTKMenuItem(str,std::dynamic_pointer_cast<GTKMenu>(parent),hasSubMenu,std::dynamic_pointer_cast<GTKMenu>(subMenu)));
    };
    NMI make_gtk_menu_seperator(){
        return NMI(new GTKMenuItem());
    };
}
