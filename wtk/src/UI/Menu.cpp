#include "omegaWTK/UI/Menu.h"

namespace OmegaWTK {

namespace {

Native::NativeMenuShortcut toNative(const MenuShortcut & sc) {
    Native::NativeMenuShortcut out;
    out.key   = sc.key;
    out.shift = sc.shift;
    out.alt   = sc.alt;
    out.meta  = sc.meta;
    return out;
}

} // namespace

MenuItem::MenuItem(const OmegaCommon::String & name,bool hasSubMenu,SharedHandle<Menu> subMenu):
    name(name),native(nullptr),hasSubMenu(hasSubMenu),subMenu(subMenu),isSeperator(false),
    itemType(Native::NativeMenuItemType::Button) {};

MenuItem::MenuItem():name(),native(nullptr),hasSubMenu(false),subMenu(nullptr),isSeperator(true),
    itemType(Native::NativeMenuItemType::Separator) {};

MenuItem::MenuItem(const OmegaCommon::String & name, Native::NativeMenuItemType type, bool initialChecked_):
    name(name),native(nullptr),hasSubMenu(false),subMenu(nullptr),isSeperator(false),
    itemType(type),initialChecked(initialChecked_) {};

void MenuItem::setParentAndInit(Menu *menu){
    parent = menu;
    switch(itemType) {
        case Native::NativeMenuItemType::Separator:
            native = Native::make_native_menu_seperator();
            break;
        case Native::NativeMenuItemType::Checkbox:
            native = Native::make_native_checkbox_item(name, parent->native, initialChecked);
            break;
        case Native::NativeMenuItemType::Radio:
            native = Native::make_native_radio_item(name, parent->native, initialChecked);
            break;
        case Native::NativeMenuItemType::Button:
        default:
            if(hasSubMenu)
                native = Native::make_native_menu_item(name,parent->native,hasSubMenu,subMenu->native);
            else
                native = Native::make_native_menu_item(name,parent->native,hasSubMenu,nullptr);
            break;
    }
};

void MenuItem::enable(){ if(native) native->setState(true); };
void MenuItem::disable(){ if(native) native->setState(false); };

void MenuItem::setTitle(const OmegaCommon::String & title){
    name = title;
    if(native) native->setTitle(title);
}

void MenuItem::setShortcut(const MenuShortcut & sc){
    if(native) native->setShortcut(toNative(sc));
}

void MenuItem::setShortcut(const OmegaCommon::String & key, bool shift, bool alt){
    MenuShortcut sc;
    sc.key = key;
    sc.shift = shift;
    sc.alt = alt;
    sc.meta = true;
    setShortcut(sc);
}

void MenuItem::setChecked(bool checked){
    initialChecked = checked;
    if(native) native->setChecked(checked);
}

bool MenuItem::isChecked() const {
    if(native) return native->isChecked();
    return initialChecked;
}

Menu::Menu(OmegaCommon::String name,std::initializer_list<SharedHandle<MenuItem>> menu_items,MenuDelegate * delegate):
name(name),
menuItems(menu_items),
native(Native::make_native_menu(name)),
delegate(delegate),
hasDelegate(delegate != nullptr){
    auto it = menu_items.begin();
    while(it != menu_items.end()){
        auto & menu_item = *it;
        menu_item->setParentAndInit(this);
        native->addMenuItem(menu_item->native);
        ++it;
    };

    if(hasDelegate) {
        delegate->menu = this;
        native->setDelegate(delegate);
    }
};

void Menu::addItem(SharedHandle<MenuItem> item){
    if(!item) return;
    item->setParentAndInit(this);
    native->addMenuItem(item->native);
    menuItems.push_back(item);
}

void Menu::insertItem(SharedHandle<MenuItem> item, unsigned idx){
    if(!item) return;
    if(idx > menuItems.size()) idx = (unsigned)menuItems.size();
    item->setParentAndInit(this);
    native->insertMenuItem(item->native, idx);
    menuItems.insert(menuItems.begin() + idx, item);
}

void Menu::removeItem(unsigned idx){
    if(idx >= menuItems.size()) return;
    native->removeMenuItem(idx);
    menuItems.erase(menuItems.begin() + idx);
}

void Menu::removeAllItems(){
    native->removeAllItems();
    menuItems.clear();
}

MenuDelegate::MenuDelegate(){};

SharedHandle<MenuItem> CategoricalMenu(const OmegaCommon::String & name,std::initializer_list<SharedHandle<MenuItem>> items,MenuDelegate *delegate){
#ifdef TARGET_WIN32
    return (SharedHandle<MenuItem>)new MenuItem(name,true,(SharedHandle<Menu>)new Menu("",items,delegate));
#endif
#ifdef TARGET_MACOS
    return (SharedHandle<MenuItem>)new MenuItem("",true,(SharedHandle<Menu>)new Menu(name,items,delegate));
#endif
#ifdef TARGET_GTK
    return (SharedHandle<MenuItem>)new MenuItem(name,true,(SharedHandle<Menu>)new Menu("",items,delegate));
#endif
};

SharedHandle<MenuItem> ButtonMenuItem(const OmegaCommon::String & name){
    return (SharedHandle<MenuItem>)new MenuItem(name,false,nullptr);
};

SharedHandle<MenuItem> SubMenu(const OmegaCommon::String & name,std::initializer_list<SharedHandle<MenuItem>> items,MenuDelegate *delegate){
    #ifdef TARGET_WIN32
        return (SharedHandle<MenuItem>) new MenuItem(name,true,(SharedHandle<Menu>)new Menu("",items,delegate));
    #endif
    #ifdef TARGET_MACOS
        return (SharedHandle<MenuItem>) new MenuItem(name,true,(SharedHandle<Menu>)new Menu("",items,delegate));
    #endif
    #ifdef TARGET_GTK
        return (SharedHandle<MenuItem>) new MenuItem(name,true,(SharedHandle<Menu>)new Menu("",items,delegate));
    #endif
};

SharedHandle<MenuItem> MenuItemSeperator(){
    return (SharedHandle<MenuItem>) new MenuItem();
};

SharedHandle<MenuItem> CheckboxMenuItem(const OmegaCommon::String & name, bool initialChecked){
    return std::make_shared<MenuItem>(name, Native::NativeMenuItemType::Checkbox, initialChecked);
}

SharedHandle<MenuItem> RadioMenuItem(const OmegaCommon::String & name, bool initialChecked){
    return std::make_shared<MenuItem>(name, Native::NativeMenuItemType::Radio, initialChecked);
}

void ShowContextMenu(SharedHandle<Menu> menu, Composition::Point2D screenPos){
    if(!menu) return;
    Native::show_native_context_menu(menu->getNativeMenu(), screenPos);
}

};

#ifdef TARGET_WIN32
#ifdef WINDOWS_PRIVATE

void select_item_on_win_menu(void * win_menu,unsigned idx){
    return __select_item_on_win_menu(win_menu, idx);
};

#endif
#endif
