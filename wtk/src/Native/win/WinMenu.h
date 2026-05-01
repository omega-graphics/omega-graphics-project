#include "omegaWTK/Native/NativeMenu.h"

#include <Windows.h>

#ifndef OMEGAWTK_NATIVE_WIN_WINMENU_H
#define OMEGAWTK_NATIVE_WIN_WINMENU_H

namespace OmegaWTK::Native {
    namespace Win {
        class WinMenuItem;
        class WinMenu : public NativeMenu {
            HMENU hmenu;
            MENUINFO info;
            friend class WinMenuItem;
            OmegaCommon::Vector<SharedHandle<WinMenuItem>> items;
            void insertMenuItem(NMI menu_item, unsigned idx) override;
            void addMenuItem(NMI menu_item) override;
            void removeMenuItem(unsigned idx) override;
            void removeAllItems() override;
            unsigned itemCount() const override { return (unsigned)items.size(); }

        public:
            HMENU getHMenu() const { return hmenu; }

            /// Returns HMENU.
            void *getNativeBinding() override {
                return hmenu;
            };
            void onSelectItem(unsigned idx){
                if(hasDelegate)
                    delegate->onSelectItem(idx);
            };
            /// Run validation against the user delegate before showing.
            /// Items whose validator returns false are greyed out.
            void runValidation();

            WinMenu(const OmegaCommon::String & name);
            ~WinMenu() override;
        };

        void select_item(void * menu,unsigned idx);

        NM make_win_menu(const OmegaCommon::String & name);
        NMI make_win_menu_item(const OmegaCommon::String & str, NM parent, bool hasSubMenu, NM subMenu);
        NMI make_win_menu_seperator();
        NMI make_win_checkbox_item(const OmegaCommon::String & str, NM parent, bool initialChecked);
        NMI make_win_radio_item(const OmegaCommon::String & str, NM parent, bool initialChecked);
        void show_win_context_menu(NM menu, Composition::Point2D screenPos);
    };
};


#endif
