#include "omegaWTK/Core/Core.h"
#include "omegaWTK/Composition/Geometry.h"

#include <cstdint>

#ifndef OMEGAWTK_NATIVE_NATIVEMENU_H
#define OMEGAWTK_NATIVE_NATIVEMENU_H

namespace OmegaWTK {
namespace Native {

struct NativeMenuShortcut {
    OmegaCommon::String key;   // Single character (lower-case), e.g. "c","q","n", or
                               // a special name: "F1".."F12","Tab","Space","Return",
                               // "Backspace","Delete","Escape","Up","Down","Left","Right",
                               // "Home","End","PageUp","PageDown".
    bool shift = false;
    bool alt   = false;
    bool meta  = true;         // Cmd on macOS; Ctrl on Win32/GTK.
};

enum class NativeMenuItemType : uint8_t {
    Button,
    Separator,
    Checkbox,
    Radio
};

class NativeMenuItem {
public:
    virtual void setState(bool state) = 0;                       // enable/disable
    virtual void setTitle(const OmegaCommon::String & title) = 0;
    virtual void setChecked(bool checked) = 0;                   // Checkbox/Radio
    virtual bool isChecked() const = 0;
    virtual void setShortcut(const NativeMenuShortcut & shortcut) = 0;
    virtual NativeMenuItemType getType() const = 0;
    virtual ~NativeMenuItem() = default;
};

typedef SharedHandle<NativeMenuItem> NMI;

class NativeMenuDelegate;

class NativeMenu {
protected:
    NativeMenuDelegate *delegate;
    bool hasDelegate = false;
public:
    void setDelegate(NativeMenuDelegate *delegate);
    virtual void *getNativeBinding() = 0;
    virtual void addMenuItem(NMI menu_item) = 0;
    virtual void insertMenuItem(NMI menu_item,unsigned idx) = 0;
    virtual void removeMenuItem(unsigned idx) = 0;
    virtual void removeAllItems() = 0;
    virtual unsigned itemCount() const = 0;
    virtual ~NativeMenu() = default;
};
typedef SharedHandle<NativeMenu> NM;


INTERFACE NativeMenuDelegate {
public:
    INTERFACE_METHOD void onSelectItem(unsigned itemIndex) ABSTRACT;
    /// Returns whether the item at @c itemIndex should be enabled for the
    /// upcoming menu display. Default: always enabled. On macOS this is wired
    /// to NSMenuValidation; on Win32/GTK it is invoked before showing a
    /// context menu.
    virtual bool onValidateItem(unsigned itemIndex) { (void)itemIndex; return true; }
};

NMI make_native_menu_item(const OmegaCommon::String & str,NM parent,bool hasSubMenu,NM subMenu);
NMI make_native_menu_seperator();
NMI make_native_checkbox_item(const OmegaCommon::String & str, NM parent, bool initialChecked = false);
NMI make_native_radio_item(const OmegaCommon::String & str, NM parent, bool initialChecked = false);
NM make_native_menu(const OmegaCommon::String & name);

/// Display @c menu as a contextual popup at @c screenPos (screen coords).
/// On Win32 the menu is displayed relative to the foreground window.
void show_native_context_menu(NM menu, Composition::Point2D screenPos);

};
};

#ifdef TARGET_WIN32
#ifdef WINDOWS_PRIVATE

void __select_item_on_win_menu(void * win_menu,unsigned idx);
#endif
#endif


#endif
