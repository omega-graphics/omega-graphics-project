#include "omegaWTK/Core/Core.h"
#include <cstdint>

#include "omegaWTK/Native/NativeEvent.h"
#include "omegaWTK/Native/NativeDialog.h"
#include "omegaWTK/Native/NativeTheme.h"
#include "omegaWTK/Native/NativeWindow.h"

#ifndef OMEGAWTK_UI_APPWINDOW_H
#define OMEGAWTK_UI_APPWINDOW_H

namespace OmegaWTK {

class Menu;

class AppWindow;
OMEGACOMMON_SHARED_CLASS(AppWindow);
class AppWindowManager;
class WidgetTreeHost;
class Widget;
OMEGACOMMON_SHARED_CLASS(Widget);
class View;
class FrameBuilder;

namespace Composition {
    class LayerTree;
}

// Widget-View-Paint-Lifecycle-Plan Tier D / D6.2 (2026-06-03):
// forward-decl for the per-window style-sheet stack. Full type in
// `omegaWTK/UI/StyleSheet.h`; the AppWindow API only needs the
// `SharedHandle<...>` instantiation, which compiles against the
// forward decl. Namespace is `StyleSheets` (plural) because the
// legacy inline-style aggregate at `omegaWTK/UI/UIView.h:19` already
// owns the `Style` identifier — see the StyleSheet.h footer comment.
namespace StyleSheets {
    class StyleSheet;
}

class AppWindowDelegate;
/**
    @brief A standard application window for attaching and displaying widgets. Similar to a Widget, it can be styled.
    @paragraph
    In order to display any widgets on this window a WidgetTreeHost must be created,
    than attached to an instance of this class.
*/
 class OMEGAWTK_EXPORT AppWindow : public Native::NativeEventEmitter,
                                    public Native::NativeThemeObserver {
        struct Impl;
        Core::UniquePtr<Impl> impl_;

        friend class AppWindowDelegate;
        friend class AppWindowManager;
        friend class WidgetTreeHost;
        friend class FrameBuilder;

        void onThemeSet(Native::ThemeDesc &desc) override;

        // --- Tier 3 Phase 3.0: window-scoped paint scaffolding ---
        // Dormant infrastructure for the FrameBuilder that lands in
        // Phase 3.1. Not on the public surface: FrameBuilder is the only
        // consumer (granted via friendship above) and the methods will
        // disappear when the per-view paint path is deleted in Tier 4.
        Composition::LayerTree * windowLayerTree() const;
        /// Tier 3 Phase 3.1: the window's frame driver. Lifetime matches
        /// AppWindow's.
        /// AppWindow-driven paint chokepoints (initWidgetTree,
        /// dispatchResize*ToHosts) bracket their tree walks with
        /// frameBuilder()->beginFrame()/endFrame() so the window-level
        /// composition session has a single owner.
        FrameBuilder * frameBuilder() const;
    public:
        OMEGACOMMON_CLASS("OmegaWTK.AppWindow")

        /// Tier 3 Phase 3.2: returns the FrameBuilder currently
        /// bracketing an AppWindow-driven paint pass, or nullptr if
        /// none is active. UIView::update / SVGView::paint read this
        /// to submit their DisplayList to the window-scoped frame.
        /// Single-threaded UI thread; not safe to call from
        /// background threads.
        static FrameBuilder * activeFrameBuilder();

        /// Widget-View-Paint-Lifecycle-Plan Tier A: request a coalesced
        /// frame flush. Schedules flushFrame() on the next run-loop turn
        /// via the native window; a burst of requests collapses to one
        /// frame. Called by the deferred Widget::invalidate path.
        void requestFrame();
        /// Build one frame: opens a single FrameBuilder ScopedFrame and
        /// repaints all dirty widgets. Invoked by the native run-loop
        /// callback registered in setRootWidget().
        void flushFrame();

        void setRootWidget(WidgetPtr widget);

    #ifndef TARGET_MOBILE
        void setMenu(SharedHandle<Menu> & menu);
        void setEnableWindowHeader(bool enable);
    #endif
#ifdef TARGET_WIN32
        SharedHandle<View> getExitButton();
        SharedHandle<View> getMaxmizeButton();
        SharedHandle<View> getMinimizeButton();
#endif
        void setTitle(OmegaCommon::StrRef title);
        void close();

        // -- Window state pass-throughs (§2.2). Cursor is intentionally
        //    NOT exposed here — the virtual hover dispatcher is the only
        //    writer of the OS cursor sink.
        void minimize();
        void maximize();
        void restore();
        void toggleFullscreen();
        bool isMinimized() const;
        bool isMaximized() const;
        bool isFullscreen() const;
        bool isVisible() const;

        /// NativeWindow-Ready-Signal-Plan §3.5(A): pass-through to
        /// `NativeWindow::isNativeReady()`. The Compositor consults
        /// this via the surface back-edge to gate render dispatch on
        /// native-surface realization. Returns true if there is no
        /// NativeWindow (matches the base interface's default).
        bool isNativeReady() const;
        Composition::Rect getRect() const;
        void setRect(const Composition::Rect & rect);
        float scaleFactor() const;
        void setMinSize(float w, float h);
        void setMaxSize(float w, float h);
        void setResizable(bool resizable);
        void orderFront();
        void orderBack();
        void setOpacity(float alpha);
        float getOpacity() const;
        bool isKeyWindow() const;
        void becomeKeyWindow();


        #ifdef TARGET_MOBILE

        SharedHandle<Native::NativeFS> openFSDialog(const Native::NativeFSDialog::Descriptor & desc);
        SharedHandle<Native::NativeNoteDialog> openNoteDialog(const Native::NativeNoteDialog::Descriptor & desc);

        #else

        SharedHandle<Native::NativeFSDialog> openFSDialog(const Native::NativeFSDialog::Descriptor & desc);
        SharedHandle<Native::NativeNoteDialog> openNoteDialog(const Native::NativeNoteDialog::Descriptor & desc);
        SharedHandle<Native::NativeAlertDialog> openAlertDialog(const Native::NativeAlertDialog::Descriptor & desc);

        #endif
        
        // Widget-View-Paint-Lifecycle-Plan Tier D / D6.2 (2026-06-03):
        // per-window stack of style sheets. The cascade in
        // `Style::StyleResolver::resolve(node)` walks the stack
        // top-to-bottom (later-added sheets win specificity ties).
        // Sheets are sharable across windows via `SharedHandle`.
        // Mutating the stack dirties root `DirtyBit::Style` and asks
        // the window to flush a frame.
        void addStyleSheet(SharedHandle<StyleSheets::StyleSheet> sheet);
        /// Removes the *first* matching handle from the stack. No-op
        /// if not present. Matches by identity (`==` on the handle),
        /// not by sheet equality.
        void removeStyleSheet(const SharedHandle<StyleSheets::StyleSheet> & sheet);
        const OmegaCommon::Vector<SharedHandle<StyleSheets::StyleSheet>> & styleSheets() const;

        explicit AppWindow(Composition::Rect rect,AppWindowDelegate * delegate = nullptr);
        ~AppWindow() override;
    };
/**
 @brief Manages the displaying of AppWindows as well as the window heirarchy for a single application.
 Desktop Apps: All windows are seperate entiityes
 Mobile Apps: All windows are share the same screen. (The transition and position is determined by)
*/
class OMEGAWTK_EXPORT  AppWindowManager : public Native::NativeThemeObserver {
        AppWindowPtr rootWindow;

        OmegaCommon::Vector<AppWindowPtr> windows;

        void closeAllWindows();

        friend class AppInst;
        void onThemeSet(Native::ThemeDesc &desc) override;
        public:
        /**
         * @brief  USE make<> to create this.
         * 
         */
        AppWindowManager();

        typedef unsigned WindowIndex;

        /**
         * @brief Add an AppWindow to the manager (No priority for it. 
         The order in which it was placed determines the priority.)
         * 
         * @param handle 
         * @return The index of the window
         */
        WindowIndex addWindow(AppWindowPtr handle);

        /**
         * @brief Set the Root Window object
         * 
         * @param handle 
         */
        void setRootWindow(AppWindowPtr handle);

        AppWindowPtr getRootWindow();

        void displayRootWindow();
        ~AppWindowManager() override = default;
    };
    
/**
 @brief An interface used for response code to a native event emitted by an AppWindow, for example when the window resizes.
 @paragraph


*/
INTERFACE OMEGAWTK_EXPORT  AppWindowDelegate : public Native::NativeEventProcessor {
    private:
        void onRecieveEvent(Native::NativeEventPtr event);
        void dispatchResizeBeginToHosts(const Composition::Rect & rect);
        void dispatchResizeToHosts(const Composition::Rect & rect);
        void dispatchResizeEndToHosts(const Composition::Rect & rect);
        void syncNativePresentLayer(const Composition::Rect & rect);
        friend class AppWindow;
        bool liveResizeActive = false;
#if defined(TARGET_MACOS)
        bool hasPendingLiveResize = false;
        Composition::Rect pendingLiveResizeRect {};
        bool hasLastDispatchedLiveResizeRect = false;
        Composition::Rect lastDispatchedLiveResizeRect {};
        std::uint64_t lastDispatchedResizeGeneration = 0;
        std::uint64_t lastResizeBeginGeneration = 0;
#endif
    protected:
       AppWindow * window;
         INTERFACE_METHOD void windowWillClose(Native::NativeEventPtr event);
        INTERFACE_METHOD void windowWillResize(Composition::Rect & nRect);
    };


}


#endif
