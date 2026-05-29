#include <gtk/gtk.h>
#include "omegaWTK/Native/NativeWindow.h"

#ifndef OMEGAWTK_NATIVE_GTK_GTKAPP_H
#define OMEGAWTK_NATIVE_GTK_GTKAPP_H

namespace OmegaWTK::Native::GTK {

GtkApplication *get_active_application();

/// Resolves the GtkWindow backing a NativeWindow handle. Returns nullptr if
/// the handle is not a GTKAppWindow. Used by dialogs to set their transient
/// parent.
GtkWindow *gtk_window_from_native(const NWH & window);

}

#endif
