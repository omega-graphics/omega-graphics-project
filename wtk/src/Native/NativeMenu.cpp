#include "omegaWTK/Native/NativeMenu.h"

#ifdef TARGET_MACOS
#include "macos/CocoaMenu.h"
#endif
#ifdef TARGET_WIN32
#include "win/WinMenu.h"
#endif
#ifdef TARGET_GTK
#include "gtk/GTKMenu.h"
#endif

namespace OmegaWTK::Native {

void NativeMenu::setDelegate(NativeMenuDelegate *delegate){
    this->delegate = delegate;
    this->hasDelegate = (delegate != nullptr);
};

NMI make_native_menu_item(const OmegaCommon::String & str,NM parent,bool hasSubMenu,NM subMenu){
#ifdef TARGET_MACOS
    return Cocoa::make_cocoa_menu_item(str,parent,hasSubMenu,subMenu);
#endif
#ifdef TARGET_WIN32
    return Win::make_win_menu_item(str,parent,hasSubMenu,subMenu);
#endif
#ifdef TARGET_GTK
    return GTK::make_gtk_menu_item(str,parent,hasSubMenu,subMenu);
#endif
    return nullptr;
};

NMI make_native_menu_seperator(){
#ifdef TARGET_MACOS
    return Cocoa::make_cocoa_menu_seperator();
#endif
#ifdef TARGET_WIN32
    return Win::make_win_menu_seperator();
#endif
#ifdef TARGET_GTK
    return GTK::make_gtk_menu_seperator();
#endif
    return nullptr;
};

NMI make_native_checkbox_item(const OmegaCommon::String & str, NM parent, bool initialChecked){
#ifdef TARGET_MACOS
    return Cocoa::make_cocoa_checkbox_item(str, parent, initialChecked);
#endif
#ifdef TARGET_WIN32
    return Win::make_win_checkbox_item(str, parent, initialChecked);
#endif
#ifdef TARGET_GTK
    return GTK::make_gtk_checkbox_item(str, parent, initialChecked);
#endif
    return nullptr;
};

NMI make_native_radio_item(const OmegaCommon::String & str, NM parent, bool initialChecked){
#ifdef TARGET_MACOS
    return Cocoa::make_cocoa_radio_item(str, parent, initialChecked);
#endif
#ifdef TARGET_WIN32
    return Win::make_win_radio_item(str, parent, initialChecked);
#endif
#ifdef TARGET_GTK
    return GTK::make_gtk_radio_item(str, parent, initialChecked);
#endif
    return nullptr;
};

NM make_native_menu(const OmegaCommon::String & name){
#ifdef TARGET_MACOS
    return Cocoa::make_cocoa_menu(name);
#endif
#ifdef TARGET_WIN32
    return Win::make_win_menu(name);
#endif
#ifdef TARGET_GTK
    return GTK::make_gtk_menu(name);
#endif
    return nullptr;
};

void show_native_context_menu(NM menu, Composition::Point2D screenPos){
#ifdef TARGET_MACOS
    Cocoa::show_cocoa_context_menu(menu, screenPos);
#endif
#ifdef TARGET_WIN32
    Win::show_win_context_menu(menu, screenPos);
#endif
#ifdef TARGET_GTK
    GTK::show_gtk_context_menu(menu, screenPos);
#endif
};

};

#ifdef TARGET_WIN32
#ifdef WINDOWS_PRIVATE

#include "win/WinMenu.h"

void __select_item_on_win_menu(void * win_menu,unsigned idx){
    return OmegaWTK::Native::Win::select_item(win_menu,idx);
};
#endif
#endif
