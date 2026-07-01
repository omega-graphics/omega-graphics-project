#include "GTKMenu.h"

#include <algorithm>
#include <iostream>
#include <memory>
#include <unordered_map>

#include <gtk/gtk.h>

// §2.15: GTKMenu is NOT ported to GTK 4. Native menus are removed entirely by
// Panels-And-Window-Customization-Plan §B3 (replaced by a virtual MenuBar
// widget), so the GtkMenu/GtkMenuBar bridge below is GTK-3-only. Under GTK 4 the
// GTK::make_gtk_menu* factories that NativeMenu.cpp dispatches to are provided as
// no-op stubs (returning null) purely to keep the link intact until §B3 deletes
// the whole NativeMenu interface.

// GTK 4 stubs — see the header comment. Menus are virtual under GTK 4 (Panels
// §B3); these exist only so NativeMenu.cpp's TARGET_GTK dispatch links.
namespace OmegaWTK::Native::GTK {
    NMI make_gtk_menu_item(const OmegaCommon::String &, NM, bool, NM){ return nullptr; }
    NMI make_gtk_menu_seperator(){ return nullptr; }
    NMI make_gtk_checkbox_item(const OmegaCommon::String &, NM, bool){ return nullptr; }
    NMI make_gtk_radio_item(const OmegaCommon::String &, NM, bool){ return nullptr; }
    NM make_gtk_menu(const OmegaCommon::String &){ return nullptr; }
    void show_gtk_context_menu(NM, Composition::Point2D){}
}

