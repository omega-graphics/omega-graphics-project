#include "omegaWTK/Native/NativeMenu.h"

#ifndef OMEGAWTK_NATIVE_MACOS_COCOAMENU_H
#define OMEGAWTK_NATIVE_MACOS_COCOAMENU_H

namespace OmegaWTK::Native::Cocoa {

NMI make_cocoa_menu_item(const OmegaCommon::String & str,NM parent,bool hasSubMenu,NM subMenu);
NMI make_cocoa_menu_seperator();
NMI make_cocoa_checkbox_item(const OmegaCommon::String & str, NM parent, bool initialChecked);
NMI make_cocoa_radio_item(const OmegaCommon::String & str, NM parent, bool initialChecked);
NM make_cocoa_menu(const OmegaCommon::String & name);
void show_cocoa_context_menu(NM menu, Composition::Point2D screenPos);

};

#endif
