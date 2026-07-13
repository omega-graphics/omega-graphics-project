#include "omegaWTK/Widgets/Overlays.h"
#include "omegaWTK/UI/UIView.h"
#include "omegaWTK/UI/View.h"
#include "omegaWTK/Composition/Brush.h"
#include "omegaWTK/Composition/Geometry.h"
#include "omegaWTK/Composition/FontEngine.h"   // TextLayoutDescriptor::Alignment / Wrapping
#include "omegaWTK/Native/NativeEvent.h"
#include "omegaWTK/Native/NativeTheme.h"
#include "../UI/WidgetTreeHost.h"               // treeHost->overlayHost() (private UI header)

#include <algorithm>
#include <cmath>

namespace OmegaWTK {

namespace {

// Menu chrome constants. Sizes are in logical dp; text widths are the same
// coarse char-cell estimate the tooltip dispatcher uses (§2.3a T1) since WTK
// has no synchronous text-measure at build time.
constexpr float kHPad        = 12.f;   // left / right inset for label + shortcut
constexpr float kShortcutGap = 28.f;   // min gap reserved between label and shortcut
constexpr float kCorner      = 6.f;    // menu + highlight corner radius
constexpr float kMinWidth    = 150.f;
constexpr float kMaxWidth    = 380.f;

struct MenuMetrics {
    Composition::Rect rect;   // menu bounds (origin 0; size = desired)
    float rowHeight = 0.f;
    float sepHeight = 0.f;
    float width     = 0.f;
    Native::ThemeDesc theme {};
};

MenuMetrics computeMetrics(const OmegaCommon::Vector<PopupMenuItem> & items){
    MenuMetrics m;
    m.theme = Native::queryCurrentTheme();
    const float fontPt = m.theme.typography.defaultSize;
    m.rowHeight = std::round(fontPt) + 16.f;               // ~ 29 dp at 13pt
    m.sepHeight = std::max(5.f, std::round(fontPt * 0.7f));
    const float charW     = fontPt * 0.62f;                // label glyph cell
    const float shortCellW = fontPt * 0.60f;               // shortcut glyph cell

    float maxContent = 0.f;
    float totalH     = 0.f;
    for(const auto & item : items){
        if(item.separator){
            totalH += m.sepHeight;
            continue;
        }
        const float titleW = charW * static_cast<float>(item.title.size());
        const float shortW = item.shortcut.empty()
            ? 0.f
            : (kShortcutGap + shortCellW * static_cast<float>(item.shortcut.size()));
        maxContent = std::max(maxContent, titleW + shortW);
        totalH += m.rowHeight;
    }

    float w = (kHPad * 2.f) + maxContent;
    w = std::min(std::max(w, kMinWidth), kMaxWidth);
    totalH = std::max(totalH, m.rowHeight);   // never a zero-height menu

    m.width = w;
    m.rect = Composition::Rect{Composition::Point2D{0.f, 0.f}, w, totalH};
    return m;
}

} // namespace

// ---------------------------------------------------------------------------
// Per-item delegate — hover highlight + click-to-activate. One instance per
// interactive (non-separator, enabled) item, installed on that item's backing
// sub-view. Moving the cursor between item sub-views fires exit on the old and
// enter on the new (WidgetTreeHost hover synthesis), so highlight tracks the
// pointer with no per-frame move handling.
// ---------------------------------------------------------------------------
class ContextMenu::ItemDelegate : public ViewDelegate {
    ContextMenu * menu_;
    int           index_;
public:
    ItemDelegate(ContextMenu * menu, int index) : menu_(menu), index_(index) {}
protected:
    void onMouseEnter(Native::NativeEventPtr) override {
        menu_->handleItemHover(index_);
    }
    void onMouseExit(Native::NativeEventPtr) override {
        // Only clear if we are still the highlighted item — a fast move to a
        // sibling has already re-highlighted by the time this exit lands.
        if(menu_->highlighted_ == index_){
            menu_->setHighlight(-1);
        }
    }
    void onLeftMouseUp(Native::NativeEventPtr) override {
        menu_->handleItemActivate(index_);
    }
};

// ---------------------------------------------------------------------------
// Root key delegate — arrow / enter navigation. Installed on the menu's root
// view, which claims focus on present so keydowns route here. Escape and Tab
// never reach this delegate: WidgetTreeHost intercepts Escape (→ overlay
// dismiss, O3) and Tab (→ focus traversal) before delegate dispatch.
// ---------------------------------------------------------------------------
class ContextMenu::KeyDelegate : public ViewDelegate {
    ContextMenu * menu_;
public:
    explicit KeyDelegate(ContextMenu * menu) : menu_(menu) {}
protected:
    void onKeyDown(Native::NativeEventPtr event) override {
        auto * kp = static_cast<Native::KeyDownParams *>(event->params);
        if(kp == nullptr){
            return;
        }
        switch(kp->code){
            case Native::KeyCode::ArrowDown: menu_->handleKeyNav(+1); break;
            case Native::KeyCode::ArrowUp:   menu_->handleKeyNav(-1); break;
            case Native::KeyCode::Enter:     menu_->handleActivateHighlighted(); break;
            default: break;
        }
    }
};

// ---------------------------------------------------------------------------
// ContextMenu
// ---------------------------------------------------------------------------

ContextMenu::ContextMenu(const OmegaCommon::Vector<PopupMenuItem> & items)
    : Widget(ViewPtr(new UIView(computeMetrics(items).rect, nullptr, "contextMenu"))),
      items_(items) {
    const MenuMetrics m = computeMetrics(items);
    theme_      = m.theme;
    rowHeight_  = m.rowHeight;
    sepHeight_  = m.sepHeight;
    menuWidth_  = m.width;
    buildContent();
    // StrongFocus (Click | Tab): a click inside the menu keeps focus on the
    // menu root (M1 click-focus walks to the nearest click-focusable ancestor)
    // rather than resigning it, so keyboard nav survives a mouse interaction.
    viewRef().setFocusPolicy(View::FocusPolicy::StrongFocus);
}

ContextMenu::~ContextMenu() = default;

void ContextMenu::onThemeSet(Native::ThemeDesc & desc){
    theme_ = desc;
    // Re-apply styles against the new theme without recreating sub-views.
    if(view != nullptr){
        auto & root = viewAs<UIView>();
        auto st = Style::Create();
        st->elementBrush("bg", Composition::ColorBrush(theme_.colors.controlBackground));
        st->elementBorder("bg", theme_.colors.separator, 1.f);
        root.setStyle(st);
    }
    for(std::size_t i = 0; i < itemViews_.size(); ++i){
        styleItem(i, static_cast<int>(i) == highlighted_);
    }
}

void ContextMenu::buildContent(){
    auto & root = viewAs<UIView>();
    const float w      = menuWidth_;
    const float totalH = rect().h;

    // Root background: a filled rounded rectangle with a hairline border.
    {
        auto & lv = root.layoutV2();
        lv.clear();
        Composition::RoundedRect bg{};
        bg.pos   = Composition::Point2D{0.f, 0.f};
        bg.w     = w;
        bg.h     = totalH;
        bg.rad_x = kCorner;
        bg.rad_y = kCorner;
        UIElementLayoutSpec spec;
        spec.tag   = "bg";
        spec.shape = Shape::RoundedRect(bg);
        lv.element(spec);
    }
    {
        auto st = Style::Create();
        st->elementBrush("bg", Composition::ColorBrush(theme_.colors.controlBackground));
        st->elementBorder("bg", theme_.colors.separator, 1.f);
        root.setStyle(st);
    }

    itemViews_.clear();
    delegates_.clear();
    itemViews_.reserve(items_.size());

    float y = 0.f;
    for(std::size_t i = 0; i < items_.size(); ++i){
        const auto & item = items_[i];
        const float h = item.separator ? sepHeight_ : rowHeight_;
        const Composition::Rect itemRect{Composition::Point2D{0.f, y}, w, h};
        // Sub-view is a UIView so each row can carry + restyle its own bg /
        // label / shortcut elements independently. Element geometry below is
        // authored in the sub-view's LOCAL space (origin {0,0}), matching the
        // Button/TextInput convention (UIView paint lifts to window space).
        auto itemView = makeSubView<UIView>(itemRect, item.separator ? "sep" : "item");
        auto & iv = *itemView;
        auto & lv = iv.layoutV2();
        lv.clear();

        if(item.separator){
            Composition::Rect rule{
                Composition::Point2D{kHPad, std::round(h * 0.5f)},
                std::max(1.f, w - (kHPad * 2.f)), 1.f};
            UIElementLayoutSpec rspec;
            rspec.tag   = "rule";
            rspec.shape = Shape::Rect(rule);
            lv.element(rspec);
        }
        else {
            // Highlight fill (transparent until hovered / selected).
            {
                UIElementLayoutSpec bgs;
                bgs.tag   = "bg";
                bgs.shape = Shape::Rect(Composition::Rect{
                    Composition::Point2D{0.f, 0.f}, w, h});
                lv.element(bgs);
            }
            // Label (left-aligned).
            {
                UIElementLayoutSpec ls;
                ls.tag      = "label";
                ls.text     = item.title;
                ls.textRect = Composition::Rect{
                    Composition::Point2D{kHPad, 0.f},
                    std::max(0.f, w - (kHPad * 2.f)), h};
                lv.element(ls);
            }
            // Shortcut (right-aligned, shares the same rect via alignment).
            if(!item.shortcut.empty()){
                UIElementLayoutSpec ss;
                ss.tag      = "shortcut";
                ss.text     = OmegaCommon::UString(item.shortcut.begin(),
                                                   item.shortcut.end());
                ss.textRect = Composition::Rect{
                    Composition::Point2D{kHPad, 0.f},
                    std::max(0.f, w - (kHPad * 2.f)), h};
                lv.element(ss);
            }

            // Interactive rows get a hover/click delegate. Disabled rows are
            // inert (greyed, no delegate) so they neither highlight nor fire.
            if(item.enabled){
                auto d = Core::UniquePtr<ViewDelegate>(
                    new ItemDelegate(this, static_cast<int>(i)));
                iv.setDelegate(d.get());
                delegates_.push_back(std::move(d));
            }
        }

        itemViews_.push_back(itemView);
        styleItem(i, /*highlighted=*/false);
        y += h;
    }

    // Root key delegate for arrow / enter navigation.
    auto kd = Core::UniquePtr<ViewDelegate>(new KeyDelegate(this));
    viewRef().setDelegate(kd.get());
    delegates_.push_back(std::move(kd));
}

void ContextMenu::styleItem(std::size_t index, bool highlighted){
    if(index >= itemViews_.size() || index >= items_.size()){
        return;
    }
    const auto & item = items_[index];
    auto & iv = static_cast<UIView &>(*itemViews_[index]);
    auto st = Style::Create();

    if(item.separator){
        st->elementBrush("rule", Composition::ColorBrush(theme_.colors.separator));
        iv.setStyle(st);
        return;
    }

    const Composition::Color clear{0.f, 0.f, 0.f, 0.f};
    st->elementBrush("bg", Composition::ColorBrush(
        highlighted ? theme_.colors.selection : clear));

    Composition::Color labelColor;
    if(!item.enabled){
        labelColor = theme_.colors.foreground.withAlpha(0.4f);
    }
    else if(highlighted){
        labelColor = Composition::Color{1.f, 1.f, 1.f, 1.f};   // contrast on selection
    }
    else {
        labelColor = theme_.colors.foreground;
    }
    st->textColor("label", labelColor);
    st->textAlignment("label", Composition::TextLayoutDescriptor::LeftCenter);
    st->textWrapping("label", Composition::TextLayoutDescriptor::None);

    if(!item.shortcut.empty()){
        const Composition::Color scColor = highlighted
            ? Composition::Color{1.f, 1.f, 1.f, 0.85f}
            : theme_.colors.foreground.withAlpha(0.55f);
        st->textColor("shortcut", scColor);
        st->textAlignment("shortcut", Composition::TextLayoutDescriptor::RightCenter);
        st->textWrapping("shortcut", Composition::TextLayoutDescriptor::None);
    }

    iv.setStyle(st);
}

void ContextMenu::setHighlight(int index){
    if(index == highlighted_){
        return;
    }
    const int prev = highlighted_;
    highlighted_ = index;

    auto restyle = [&](int i){
        if(i < 0 || i >= static_cast<int>(itemViews_.size())){
            return;
        }
        styleItem(static_cast<std::size_t>(i), i == index);
        itemViews_[static_cast<std::size_t>(i)]->markDirty(View::Style | View::Paint);
    };
    restyle(prev);
    restyle(index);

    // Force the overlay to re-resolve styles this frame and schedule it. The
    // per-subview mark handles the row that changed; marking the root Style
    // guarantees the pre-order Style walker descends into it.
    viewRef().markDirty(View::Style | View::Paint);
    invalidate();
}

int ContextMenu::stepEnabled(int start, int dir) const {
    const int n = static_cast<int>(items_.size());
    if(n == 0){
        return -1;
    }
    // Seed so the first step lands on the natural entry point: index 0 for a
    // downward move from "nothing selected", the last item for an upward one.
    int idx = (start < 0) ? (dir > 0 ? -1 : 0) : start;
    for(int step = 0; step < n; ++step){
        idx = (((idx + dir) % n) + n) % n;
        const auto & it = items_[static_cast<std::size_t>(idx)];
        if(!it.separator && it.enabled){
            return idx;
        }
    }
    return -1;   // no enabled item anywhere
}

void ContextMenu::handleItemHover(int index){
    if(index < 0 || index >= static_cast<int>(items_.size())){
        return;
    }
    const auto & item = items_[static_cast<std::size_t>(index)];
    if(item.separator || !item.enabled){
        return;
    }
    setHighlight(index);
}

void ContextMenu::handleItemActivate(int index){
    if(index < 0 || index >= static_cast<int>(items_.size())){
        return;
    }
    const auto & item = items_[static_cast<std::size_t>(index)];
    if(item.separator || !item.enabled){
        return;
    }
    // dismiss() releases the overlay host's reference to this menu, which may
    // be the last one — hold ourselves alive across the callback, and copy the
    // action out before tearing down (the item vector dies with us).
    auto keepAlive = shared_from_this();
    auto action = item.action;
    dismiss();
    if(action){
        action();
    }
}

void ContextMenu::handleKeyNav(int delta){
    const int next = stepEnabled(highlighted_, delta > 0 ? 1 : -1);
    if(next >= 0){
        setHighlight(next);
    }
}

void ContextMenu::handleActivateHighlighted(){
    if(highlighted_ >= 0){
        handleItemActivate(highlighted_);
    }
}

void ContextMenu::present(Widget * anchor, Composition::Point2D windowPos){
    if(anchor == nullptr || handle_.valid()){
        return;
    }
    // Reach the window's overlay layer through the anchor's host (friend
    // access — Widget::treeHost is protected and the anchor is a base
    // Widget*). A detached anchor has no host to present into.
    WidgetTreeHost * host = anchor->treeHost;
    if(host == nullptr){
        return;
    }
    presentedHost_ = host;

    OverlayAnchor anchorSpec;
    anchorSpec.mode  = OverlayAnchor::Mode::AtPoint;   // context menus open at the cursor
    anchorSpec.point = windowPos;

    OverlayDismissPolicy policy;   // defaults: click-outside + Escape dismiss, absorbs hits
    OverlayOrnamentation ornament;
    ornament.cornerRadius = kCorner;

    // present() stores `self` in the OverlayHost entry, keeping the menu alive
    // while shown, so the caller may drop its handle afterward.
    WidgetPtr self = shared_from_this();
    handle_ = host->overlayHost().present(
        self, OverlayTier::Floating, anchorSpec, policy, ornament);

    // The overlay is now mounted (its views have a host), so claim keyboard
    // focus. FocusReason::Popup is a keyboard reason, so O4's restore returns
    // the ring to the opener on dismiss. O4 already captured the prior focus
    // holder inside present() above.
    highlighted_ = -1;
    viewRef().focus(FocusReason::Popup);
}

void ContextMenu::dismiss(){
    if(!handle_.valid() || presentedHost_ == nullptr){
        return;
    }
    // DEFERRED teardown. dismiss() is typically called from inside an item's
    // click handler (or the Enter key handler) — i.e. while the input
    // dispatch running that delegate is still on the stack. Destroying the
    // overlay now would free the very View + delegate being dispatched
    // (use-after-free). `requestDeferredDismiss` queues it; the dispatcher
    // drains the queue once the event has fully unwound. Clear our presented
    // state first so a re-entrant dismiss is a no-op; the OverlayHost keeps
    // this menu alive until the drain, so no local keep-alive is needed.
    const OverlayHandle h = handle_;
    WidgetTreeHost * host = presentedHost_;
    handle_        = OverlayHandle{};
    presentedHost_ = nullptr;
    host->overlayHost().requestDeferredDismiss(h);
}

}
