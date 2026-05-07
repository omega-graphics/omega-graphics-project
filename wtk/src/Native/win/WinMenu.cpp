#include "WinMenu.h"

#include "NativePrivate/win/WinUtils.h"

#include <memory>
#include <sstream>
#include <windowsx.h>

namespace OmegaWTK::Native::Win {

    namespace {
        /// Build the display string Windows shows in a menu, including the
        /// "\tCtrl+Shift+X" suffix that triggers right-aligned shortcut text.
        std::wstring composeMenuText(const std::wstring & label, const NativeMenuShortcut * sc) {
            if(!sc || sc->key.empty()) return label;
            std::wstringstream ss;
            ss << label << L"\t";
            if(sc->meta)  ss << L"Ctrl+";
            if(sc->alt)   ss << L"Alt+";
            if(sc->shift) ss << L"Shift+";
            std::wstring k;
            cpp_str_to_cpp_wstr(sc->key, k);
            // Capitalize first letter for display.
            if(!k.empty() && k[0] >= L'a' && k[0] <= L'z') k[0] = (wchar_t)(k[0] - L'a' + L'A');
            ss << k;
            return ss.str();
        }
    }

    class WinMenuItem : public NativeMenuItem {
        MENUITEMINFOW info{};
        WinMenu *subMenu;
        bool hasSubMenu;
        WinMenu *parent;
        unsigned idx = 0;
        NativeMenuItemType type = NativeMenuItemType::Button;
        bool checkedState = false;
        std::wstring labelW;            // Backing store for info.dwTypeData.
        std::wstring displayW;          // labelW + shortcut suffix; pointed to by info.dwTypeData.
        OmegaCommon::String labelUtf8;  // Source-of-truth label.
        NativeMenuShortcut shortcut;
        bool hasShortcut = false;
        friend class WinMenu;

        void rebuildDisplay() {
            displayW = composeMenuText(labelW, hasShortcut ? &shortcut : nullptr);
            info.dwTypeData = const_cast<wchar_t *>(displayW.c_str());
            info.cch = (UINT)displayW.size();
            if(parent) {
                SetMenuItemInfoW(parent->getHMenu(), idx, TRUE, &info);
            }
        }

    public:
        void assignIndex(unsigned _idx){ idx = _idx; }

        void setState(bool state) override {
            UINT mask = MF_BYPOSITION;
            if(state) mask |= MF_ENABLED;
            else      mask |= MF_GRAYED | MF_DISABLED;
            if(parent) EnableMenuItem(parent->getHMenu(), idx, mask);
        }

        void setTitle(const OmegaCommon::String & title) override {
            labelUtf8 = title;
            cpp_str_to_cpp_wstr(labelUtf8, labelW);
            info.fMask |= MIIM_STRING;
            rebuildDisplay();
        }

        void setChecked(bool checked) override {
            if(type != NativeMenuItemType::Checkbox && type != NativeMenuItemType::Radio) return;
            checkedState = checked;
            if(parent) {
                CheckMenuItem(parent->getHMenu(), idx,
                              MF_BYPOSITION | (checked ? MF_CHECKED : MF_UNCHECKED));
            }
        }

        bool isChecked() const override { return checkedState; }

        void setShortcut(const NativeMenuShortcut & sc) override {
            shortcut = sc;
            hasShortcut = !sc.key.empty();
            info.fMask |= MIIM_STRING;
            rebuildDisplay();
        }

        NativeMenuItemType getType() const override { return type; }

        /// Constructs a Button menu item. Initialises @c info only; the parent
        /// menu's add/insert calls SetMenuItemInfo.
        WinMenuItem(const OmegaCommon::String & name, WinMenu * parent, bool hasSubMenu_, WinMenu * subMenu_);

        /// Constructs a Separator.
        WinMenuItem() : subMenu(nullptr), hasSubMenu(false), parent(nullptr) {
            info.cbSize = sizeof(info);
            info.fMask = MIIM_FTYPE;
            info.fType = MFT_SEPARATOR;
            type = NativeMenuItemType::Separator;
        }

        /// Constructs a Checkbox/Radio item.
        WinMenuItem(const OmegaCommon::String & name, NativeMenuItemType t, bool initialChecked)
            : subMenu(nullptr), hasSubMenu(false), parent(nullptr),
              type(t), checkedState(initialChecked), labelUtf8(name) {
            cpp_str_to_cpp_wstr(labelUtf8, labelW);
            displayW = labelW;
            info.cbSize = sizeof(info);
            info.dwItemData = (ULONG_PTR)this;
            info.fMask = MIIM_STRING | MIIM_DATA | MIIM_STATE | MIIM_FTYPE;
            // MFT_RADIOCHECK draws a bullet instead of a check.
            info.fType = (t == NativeMenuItemType::Radio) ? MFT_RADIOCHECK : MFT_STRING;
            info.fState = MFS_ENABLED | (initialChecked ? MFS_CHECKED : MFS_UNCHECKED);
            // Use Win Utils wstring_to_str and str_to_wstring helpers.
            info.dwTypeData = const_cast<wchar_t *>(displayW.c_str());
            info.cch = (UINT)displayW.size();
        }

        ~WinMenuItem() override = default;
    };

    WinMenuItem::WinMenuItem(const OmegaCommon::String & name, WinMenu * parent_, bool hasSubMenu_, WinMenu * subMenu_)
        : subMenu(subMenu_), hasSubMenu(hasSubMenu_), parent(parent_), labelUtf8(name) {
        cpp_str_to_cpp_wstr(labelUtf8, labelW);
        displayW = labelW;
        info.cbSize = sizeof(info);
        info.dwItemData = (ULONG_PTR)this;
        info.fMask = MIIM_STRING | MIIM_DATA | MIIM_STATE;
        if(hasSubMenu) info.fMask |= MIIM_SUBMENU;
        info.dwTypeData = const_cast<wchar_t *>(displayW.c_str());
        info.cch = (UINT)displayW.size();
        info.fState = MFS_ENABLED;
        if(hasSubMenu) info.hSubMenu = subMenu->getHMenu();
    }

    // ----------------------------------------------------------------------

    WinMenu::WinMenu(const OmegaCommon::String & name) {
        (void)name;
        info.dwMenuData = (ULONG_PTR)this;
        info.cbSize = sizeof(info);
        info.fMask = MIM_MENUDATA | MIM_BACKGROUND | MIM_STYLE;
        info.dwStyle = MNS_NOTIFYBYPOS;
        info.hbrBack = (HBRUSH)COLOR_WINDOW;
        hmenu = CreateMenu();
        SetMenuInfo(hmenu, &info);
    }

    WinMenu::~WinMenu() {
        if(hmenu) {
            DestroyMenu(hmenu);
            hmenu = nullptr;
        }
    }

    void WinMenu::insertMenuItem(NMI menu_item, unsigned int idx) {
        auto item = std::dynamic_pointer_cast<WinMenuItem>(menu_item);
        if(!item) return;
        if(idx > items.size()) idx = (unsigned)items.size();
        item->parent = this;
        InsertMenuItemW(hmenu, idx, TRUE, &item->info);
        items.insert(items.begin() + idx, item);
        for(unsigned i = 0; i < items.size(); ++i) items[i]->idx = i;
    }

    void WinMenu::addMenuItem(NMI menu_item) {
        auto item = std::dynamic_pointer_cast<WinMenuItem>(menu_item);
        if(!item) return;
        item->parent = this;
        item->idx = (unsigned)items.size();
        InsertMenuItemW(hmenu, (UINT)items.size(), TRUE, &item->info);
        items.push_back(item);
    }

    void WinMenu::removeMenuItem(unsigned idx) {
        if(idx >= items.size()) return;
        RemoveMenu(hmenu, idx, MF_BYPOSITION);
        items.erase(items.begin() + idx);
        for(unsigned i = 0; i < items.size(); ++i) items[i]->idx = i;
    }

    void WinMenu::removeAllItems() {
        // Removing from the back avoids index shifting churn.
        for(int i = (int)items.size() - 1; i >= 0; --i) {
            RemoveMenu(hmenu, (UINT)i, MF_BYPOSITION);
        }
        items.clear();
    }

    void WinMenu::runValidation() {
        if(!hasDelegate) return;
        for(unsigned i = 0; i < items.size(); ++i) {
            bool enabled = delegate->onValidateItem(i);
            EnableMenuItem(hmenu, i,
                           MF_BYPOSITION | (enabled ? MF_ENABLED : (MF_GRAYED | MF_DISABLED)));
        }
    }

    void select_item(void * menu, unsigned idx){
        WinMenu *_menu = (WinMenu *)menu;
        _menu->onSelectItem(idx);
    }

    NM make_win_menu(const OmegaCommon::String & name){
        return std::make_shared<WinMenu>(name);
    }
    NMI make_win_menu_item(const OmegaCommon::String & str, NM parent, bool hasSubMenu, NM subMenu){
        return std::make_shared<WinMenuItem>(str, (WinMenu *)parent.get(), hasSubMenu, (WinMenu *)subMenu.get());
    }
    NMI make_win_menu_seperator(){
        return std::make_shared<WinMenuItem>();
    }
    NMI make_win_checkbox_item(const OmegaCommon::String & str, NM parent, bool initialChecked){
        (void)parent;
        return std::make_shared<WinMenuItem>(str, NativeMenuItemType::Checkbox, initialChecked);
    }
    NMI make_win_radio_item(const OmegaCommon::String & str, NM parent, bool initialChecked){
        (void)parent;
        return std::make_shared<WinMenuItem>(str, NativeMenuItemType::Radio, initialChecked);
    }
    void show_win_context_menu(NM menu, Composition::Point2D screenPos){
        auto wm = std::dynamic_pointer_cast<WinMenu>(menu);
        if(!wm || !wm->getHMenu()) return;
        wm->runValidation();
        // TrackPopupMenuEx requires an owner window for input forwarding; use
        // the foreground window as a sensible default for a context menu.
        HWND owner = GetForegroundWindow();
        // TPM_RETURNCMD would return the chosen id, but we rely on
        // WM_MENUCOMMAND -> select_item routing (MNS_NOTIFYBYPOS style).
        TrackPopupMenuEx(wm->getHMenu(),
                         TPM_LEFTALIGN | TPM_TOPALIGN,
                         (int)screenPos.x, (int)screenPos.y,
                         owner, nullptr);
    }
};
