#include "omegaWTK/UI/Widget.h"
#include "omegaWTK/Native/NativeItem.h"

#ifndef OMEGAWTK_UI_NATIVEVIEWHOST_H
#define OMEGAWTK_UI_NATIVEVIEWHOST_H

namespace OmegaWTK {

/**
 @brief Escape hatch for embedding a real native view (NSView, HWND,
 GtkWidget) inside the virtual widget tree.

 This is the only path for creating native subviews. Standard widgets
 (labels, buttons, containers, separators, shapes) should never need
 a native view. Use NativeViewHost only when platform integration
 requires it: text input fields (IME), web content views, video
 players, or platform-specific controls.

 The embedded native view is attached as a child of the window's root
 native view. Its bounds are synchronized with this widget's position
 in the virtual tree. Native views render on top of virtual content
 (the "airspace" problem).

 @see Widget
*/
class OMEGAWTK_EXPORT NativeViewHost : public Widget {
    Native::NativeItemPtr embeddedItem_;
    bool attached_ = false;

    void syncBounds();
public:
    explicit NativeViewHost(Composition::Rect rect);
    ~NativeViewHost() override;

    /// Attach a real native view. The backend AppWindow will add it
    /// as a child of its internal root native view. Bounds are
    /// synchronized with this widget's position in the virtual tree.
    /// If called before the widget is mounted, attachment is deferred
    /// until onMount().
    void attach(Native::NativeItemPtr nativeItem);

    /// Remove the embedded native view from the window's root native
    /// view. The NativeItemPtr is released.
    void detach();

    /// Returns true if a native item is currently embedded.
    bool hasAttachedItem() const;

protected:
    void onMount() override;
    void onLayoutResolved(const Composition::Rect & finalRectPx) override;
};

}

#endif
