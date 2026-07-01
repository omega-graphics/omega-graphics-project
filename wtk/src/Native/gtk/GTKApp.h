#include <gtk/gtk.h>
#include "omegaWTK/Native/NativeWindow.h"

#ifndef OMEGAWTK_NATIVE_GTK_GTKAPP_H
#define OMEGAWTK_NATIVE_GTK_GTKAPP_H

namespace OmegaWTK::Native::GTK {

class SurfaceHost;

GtkApplication *get_active_application();

/// Resolves the GtkWindow backing a NativeWindow handle. Returns nullptr if
/// the handle is not a GTKAppWindow. Used by dialogs to set their transient
/// parent.
GtkWindow *gtk_window_from_native(const NWH & window);

/// Returns the per-window SurfaceHost (X11 or Wayland, per the runtime-
/// detected backend — §2.13a). Used by the Linux Vulkan visual tree (§2.14)
/// and NativeViewHost child-surface factories to allocate child surfaces
/// underneath the toplevel. Returns nullptr if the handle is not a
/// GTKAppWindow or backend detection failed.
SurfaceHost *surface_host_from_native(const NWH & window);

/// Top inset (in DIPs) consumed by the GTK menu bar, if a menu is
/// attached. Used by the WidgetTreeHost hover dispatcher (§2.3a) to
/// translate incoming event coordinates into the virtual view-tree's
/// coordinate space. Returns 0 when no menu is attached or for non-GTK
/// windows.
float gtk_menu_bar_inset_from_native(const NWH & window);

}

#endif
