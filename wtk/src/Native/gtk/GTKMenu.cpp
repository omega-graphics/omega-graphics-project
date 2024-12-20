#include "GTKMenu.h"

#include <iostream>

#include <gtk/gtk.h>

namespace OmegaWTK::Native::GTK {
    class GTKMenu;

    class GTKMenuItem : public NativeMenuItem {
        GtkMenuItem *ptr;
        SharedHandle<GTKMenu> parent;
        friend class GTKMenu;
        bool hasSubMenu;
        SharedHandle<GTKMenu> subMenu;
        static void selectThisItem(SharedHandle<GTKMenuItem> ptr);
    public:
        void setState(bool state){

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
        };
        ~GTKMenuItem();
    };

    class GTKMenu : public NativeMenu {
        GtkMenuShell *ptr; 
        OmegaCommon::Vector<SharedHandle<GTKMenuItem>> items;
        friend class GTKMenuItem;
        void onSelectItem(SharedHandle<GTKMenuItem> item){
            auto item_it = items.begin();
            unsigned idx = 0;
            while(item_it != items.end()){
                if((*item_it) == item){
                    break;
                };
                ++item_it;
                ++idx;
            };
            if(idx == items.size()){
                std::cout << "Menu Item Not added to Menu... Will not select" << std::endl;
                return;
            };

            if(hasDelegate){
                delegate->onSelectItem(idx);
            };

        };
    public:
        GTKMenu(const OmegaCommon::String & name){
            ptr = GTK_MENU_SHELL(gtk_menu_new());
        };
        void addMenuItem(NMI menu_item){
            auto item = std::dynamic_pointer_cast<GTKMenuItem>(menu_item);
            gtk_menu_shell_append(ptr,GTK_WIDGET(item->ptr));
            items.push_back(item);
        };
        void insertMenuItem(NMI menu_item, unsigned int idx){
            auto item = std::dynamic_pointer_cast<GTKMenuItem>(menu_item);
            gtk_menu_shell_insert(ptr,GTK_WIDGET(item->ptr),idx);
            items.insert(items.begin() + idx,item);
        };
        void * getNativeBinding(){
            return (void *)ptr;
        };
        ~GTKMenu(){
            g_object_unref(ptr);
            // auto it = items.begin();
            // while(it != items.end()){
            //     auto item = *it;
            //     delete item;
            //     ++it;
            // };
        };
    };

    GTKMenuItem::GTKMenuItem(const OmegaCommon::String &str, SharedHandle<GTKMenu> parent, bool hasSubMenu, SharedHandle<GTKMenu> subMenu):parent(parent),hasSubMenu(hasSubMenu),subMenu(subMenu){
        ptr = GTK_MENU_ITEM(gtk_menu_item_new_with_label(str.c_str()));
        
        gtk_menu_item_set_submenu(ptr,GTK_WIDGET(subMenu->ptr));
        /// Connect the callback to the menuitem
        g_signal_connect_swapped(ptr,"activate",G_CALLBACK(selectThisItem),(gpointer)this);
    };

    void GTKMenuItem::selectThisItem(SharedHandle<GTKMenuItem> ptr){
        ptr->parent->onSelectItem(ptr);
    };

    GTKMenuItem::~GTKMenuItem(){
        g_object_unref(ptr);
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