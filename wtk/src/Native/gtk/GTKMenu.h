#include "omegaWTK/Native/NativeMenu.h"

#ifndef OMEGAWTK_NATIVE_GTK_GTKMENU_H
#define OMEGAWTK_NATIVE_GTK_GTKMENU_H

namespace OmegaWTK::Native::GTK {

NMI make_gtk_menu_item(const OmegaCommon::String & str,NM parent,bool hasSubMenu,NM subMenu);
NMI make_gtk_menu_seperator();
NMI make_gtk_checkbox_item(const OmegaCommon::String & str, NM parent, bool initialChecked);
NMI make_gtk_radio_item(const OmegaCommon::String & str, NM parent, bool initialChecked);
NM make_gtk_menu(const OmegaCommon::String & name);
void show_gtk_context_menu(NM menu, Composition::Point2D screenPos);

};

#endif
