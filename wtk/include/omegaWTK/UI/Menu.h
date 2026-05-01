#include "omegaWTK/Core/Core.h"
#include "omegaWTK/Native/NativeMenu.h"


#ifndef OMEGAWTK_UI_MENU_H
#define OMEGAWTK_UI_MENU_H

namespace OmegaWTK {

class Menu;

struct MenuShortcut {
    OmegaCommon::String key;   // See NativeMenuShortcut for accepted strings.
    bool shift = false;
    bool alt   = false;
    bool meta  = true;         // Cmd on macOS; Ctrl on Win32/GTK.
};

class OMEGAWTK_EXPORT  MenuItem {
    OmegaCommon::String name;
    Native::NMI native;
    SharedHandle<Menu> subMenu;
    bool hasSubMenu;
    Menu *parent;
    friend class Menu;
    void setParentAndInit(Menu *menu);
    bool isSeperator;
    Native::NativeMenuItemType itemType = Native::NativeMenuItemType::Button;
    bool initialChecked = false;
public:
    OMEGACOMMON_CLASS("OmegaWTK.UI.MenuItem")
    void disable();
    void enable();

    /// Set the visible label for this item.
    void setTitle(const OmegaCommon::String & title);
    /// Set a keyboard shortcut. Pass an empty `key` to clear.
    void setShortcut(const MenuShortcut & shortcut);
    /// Convenience: set a shortcut with the platform-appropriate accel modifier.
    void setShortcut(const OmegaCommon::String & key, bool shift = false, bool alt = false);
    /// Toggle the check state for Checkbox/Radio items.
    void setChecked(bool checked);
    bool isChecked() const;
    Native::NativeMenuItemType getType() const { return itemType; }

    /// Constructs a normal menu item.
    MenuItem(const OmegaCommon::String & name,bool hasSubMenu,SharedHandle<Menu> subMenu);
    /// Constructs a separator.
    MenuItem();
    /// Constructs a checkbox or radio item.
    MenuItem(const OmegaCommon::String & name, Native::NativeMenuItemType type, bool initialChecked);
//    ~MenuItem();
};

class MenuDelegate;

 class OMEGAWTK_EXPORT  Menu {
    OmegaCommon::String name;
    OmegaCommon::Vector<SharedHandle<MenuItem>> menuItems;
    Native::NM native;
    MenuDelegate *delegate;
    bool hasDelegate;
    friend class MenuItem;
public:
    OMEGACOMMON_CLASS("OmegaWTK.UI.Menu")

    Native::NM getNativeMenu(){ return native;};

    SharedHandle<MenuItem> & getItemByIdx(unsigned idx){ return menuItems[idx];};
    unsigned itemCount() const { return (unsigned)menuItems.size(); }

    /// Append an item to this menu. The item is initialised against this menu
    /// before being attached natively.
    void addItem(SharedHandle<MenuItem> item);
    /// Insert an item at @c idx (clamped to the current count).
    void insertItem(SharedHandle<MenuItem> item, unsigned idx);
    void removeItem(unsigned idx);
    void removeAllItems();

    Menu(OmegaCommon::String name,std::initializer_list<SharedHandle<MenuItem>> menu_items,MenuDelegate *delegate = nullptr);
//    ~Menu();
};

INTERFACE OMEGAWTK_EXPORT MenuDelegate : public Native::NativeMenuDelegate {
protected:
    Menu *menu;
    friend class Menu;
public:
    MenuDelegate();
    INTERFACE_METHOD void onSelectItem(unsigned itemIndex) ABSTRACT;
    /// Optional: return false to grey out the item for the next display cycle.
    /// Default returns true. Wired to NSMenuValidation on macOS; called before
    /// context menu display on Win32/GTK.
    bool onValidateItem(unsigned itemIndex) override { (void)itemIndex; return true; }
};
/**
 Creates a Category Menu
 */
OMEGAWTK_EXPORT SharedHandle<MenuItem> CategoricalMenu(const OmegaCommon::String & name,std::initializer_list<SharedHandle<MenuItem>> items,MenuDelegate *delegate = nullptr);
/**
 Creates a SubMenu under a Categorical Menu
 */
OMEGAWTK_EXPORT SharedHandle<MenuItem> SubMenu(const OmegaCommon::String & name,std::initializer_list<SharedHandle<MenuItem>> items,MenuDelegate *delegate = nullptr);

OMEGAWTK_EXPORT SharedHandle<MenuItem> ButtonMenuItem(const OmegaCommon::String & name);
/**
 Creates a Menu Seperator Item!
*/
OMEGAWTK_EXPORT SharedHandle<MenuItem> MenuItemSeperator();

/// Creates a checkbox menu item. Group exclusivity is the caller's responsibility.
OMEGAWTK_EXPORT SharedHandle<MenuItem> CheckboxMenuItem(const OmegaCommon::String & name,
                                                       bool initialChecked = false);

/// Creates a radio menu item. Group exclusivity is the caller's responsibility.
OMEGAWTK_EXPORT SharedHandle<MenuItem> RadioMenuItem(const OmegaCommon::String & name,
                                                    bool initialChecked = false);

/// Show @c menu as a contextual popup at screen-coordinate @c screenPos.
OMEGAWTK_EXPORT void ShowContextMenu(SharedHandle<Menu> menu, Composition::Point2D screenPos);

};


#ifdef TARGET_WIN32
#ifdef WINDOWS_PRIVATE

OMEGAWTK_EXPORT void select_item_on_win_menu(void * win_menu,unsigned idx);

#endif
#endif

#endif
