
#include "omegaWTK/UI/Widget.h"
#include "omegaWTK/UI/OverlayHost.h"   // OverlayHandle stored per presented menu
#include "omegaWTK/Native/NativeTheme.h" // ThemeDesc cached for restyle-on-highlight

#include <functional>
#include <memory>

#ifndef OMEGAWTK_WIDGETS_OVERLAYS_H
#define OMEGAWTK_WIDGETS_OVERLAYS_H

namespace OmegaWTK {

class ViewDelegate;

/**
 * @brief One entry in a `ContextMenu` / `PopupMenu`.
 *
 * `title` is the command label; `shortcut` is an optional right-aligned
 * accelerator hint (display only — the menu does not bind it). `action`
 * fires when the item is chosen by click or Enter. A disabled item is
 * greyed and non-interactive. A `separator` item renders as a thin rule
 * and ignores every other field. (The catalog's nested `submenu` is
 * modeled in the plan but not yet built — see Overlays.cpp.)
 */
struct OMEGAWTK_EXPORT PopupMenuItem {
    OmegaCommon::UString  title    {};
    OmegaCommon::String   shortcut {};
    std::function<void()> action    = nullptr;
    bool                  enabled   = true;
    bool                  separator = false;
};

/**
 * @brief In-view command menu presented through the window's `OverlayHost`.
 *
 * Widget-Stub-Implementation-Plan Phase 6D. Unlike the native `Menu`
 * (`Menu.h`, backed by `NativeMenu`), this is a fully in-view-rendered
 * menu so it can be themed and styled like any other widget. It is a
 * `Widget` whose root `UIView` is a vertical list; each item is a child
 * sub-view with its own hover/click delegate. It presents into
 * `OverlayTier::Floating`, so it paints above the main tree, absorbs
 * hits, dismisses on click-outside / Escape (Overlay-Z-Order-Plan O3),
 * and returns focus to the opener on dismiss (O4).
 *
 * Construct with `make<ContextMenu>(items)` and present relative to an
 * in-tree anchor:
 * @code
 *   auto menu = make<ContextMenu>(items);
 *   menu->present(anchorWidget, cursorPosInWindowSpace);
 * @endcode
 * The `OverlayHost` owns the menu while it is shown, so the caller may
 * drop its handle after `present`. Keyboard: Up / Down move the
 * highlight, Enter activates, Escape closes (handled by the host).
 */
class OMEGAWTK_EXPORT ContextMenu
    : public Widget,
      public std::enable_shared_from_this<ContextMenu> {
    class ItemDelegate;   ///< per-item hover + click (defined in Overlays.cpp)
    class KeyDelegate;    ///< arrow / enter nav on the menu root
    friend class ItemDelegate;
    friend class KeyDelegate;

    OmegaCommon::Vector<PopupMenuItem>                 items_;
    Native::ThemeDesc                                  theme_ {};
    /// One backing UIView sub-view per entry in `items_` (index-aligned),
    /// including separators (which get a non-interactive rule sub-view).
    OmegaCommon::Vector<ViewPtr>                       itemViews_;
    /// Owned event delegates: one `ItemDelegate` per interactive item plus
    /// the single `KeyDelegate` on the root. Stored as base pointers so the
    /// concrete types stay private to Overlays.cpp.
    OmegaCommon::Vector<Core::UniquePtr<ViewDelegate>> delegates_;
    /// Index into `items_` of the currently highlighted item, or -1.
    int                                                highlighted_ = -1;
    /// Live handle while presented (invalid otherwise) and the host we
    /// presented through — kept so `dismiss()` reaches the same
    /// `OverlayHost` without re-deriving it from an anchor.
    OverlayHandle                                      handle_ {};
    WidgetTreeHost *                                   presentedHost_ = nullptr;
    float                                              rowHeight_ = 0.f;
    float                                              sepHeight_ = 0.f;
    float                                              menuWidth_ = 0.f;

    void buildContent();
    void styleItem(std::size_t index, bool highlighted);
    void setHighlight(int index);
    int  stepEnabled(int start, int dir) const;
    void handleItemHover(int index);
    void handleItemActivate(int index);
    void handleKeyNav(int delta);
    void handleActivateHighlighted();
protected:
    /// Re-theme in place on an OS / app theme change: refresh the cached
    /// `ThemeDesc` and re-apply the root + per-row styles without rebuilding
    /// the sub-views.
    void onThemeSet(Native::ThemeDesc & desc) override;
public:
    explicit ContextMenu(const OmegaCommon::Vector<PopupMenuItem> & items);
    ~ContextMenu() override;

    /// Present the menu at `windowPos` (window-space) with `anchor`'s
    /// `WidgetTreeHost` as the host. `anchor` must be a widget currently
    /// in the tree (it supplies the overlay layer); no-op if it is null
    /// or detached. Claims keyboard focus so arrow / Enter navigation
    /// works immediately.
    void present(Widget * anchor, Composition::Point2D windowPos);

    /// Take the menu down if it is presented. Safe to call when it is not.
    void dismiss();

    /// Whether the menu is currently presented.
    bool isPresented() const { return handle_.valid(); }
};

}

#endif // OMEGAWTK_WIDGETS_OVERLAYS_H
