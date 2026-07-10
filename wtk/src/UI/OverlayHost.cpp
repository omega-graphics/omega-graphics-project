#include "omegaWTK/UI/OverlayHost.h"
#include "omegaWTK/UI/Widget.h"
#include "omegaWTK/UI/View.h"
#include "omegaWTK/UI/AppWindow.h"
#include "WidgetTreeHost.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

namespace OmegaWTK {

namespace {

constexpr std::size_t kTierCount = 4;

constexpr std::size_t tierIndex(OverlayTier tier){
    // Floating=1 → 0, Modal=2 → 1, Tooltip=3 → 2, DragGhost=4 → 3.
    // Anything outside the enum clamps to Floating so a corrupted
    // input does not run off the end of the per-tier vectors.
    const auto raw = static_cast<std::uint8_t>(tier);
    if(raw < 1 || raw > kTierCount){
        return 0;
    }
    return static_cast<std::size_t>(raw - 1);
}

/// Window-local origin of `widget`. `View::offsetFromRoot()` already
/// sums each ancestor's `rect.pos + parent.contentOffset()`, so the
/// result is the widget's top-left relative to the WidgetTreeHost
/// root — which is what overlay coordinates are expressed in.
///
/// Contract: `widget` came from a real `Widget` constructor (either
/// `Widget(Composition::Rect)` or `Widget(ViewPtr)` with a non-null
/// view). Both set `view`; the only path to a view-less Widget is a
/// caller explicitly passing nullptr to `Widget(ViewPtr)`, which
/// produces something that cannot render, hit-test, or anchor — not
/// a meaningful overlay input. The public `viewRef()` accessor is
/// used (not the protected `view` field) because this helper lives
/// in an anonymous namespace, not inside `OverlayHost` itself.
Composition::Point2D widgetWindowOrigin(Widget * widget){
    if(widget == nullptr){
        return {0.f, 0.f};
    }
    return widget->viewRef().offsetFromRoot();
}

/// Half-open rect containment, matching `View::containsPoint`
/// (View.Core.cpp:182): the right / bottom edges are exclusive so a
/// point exactly on the far edge of an overlay counts as *outside*.
bool rectContainsPoint(const Composition::Rect & r,
                       const Composition::Point2D & p){
    return p.x >= r.pos.x && p.x < r.pos.x + r.w &&
           p.y >= r.pos.y && p.y < r.pos.y + r.h;
}

} // namespace

struct OverlayHost::Impl {
    /// O3 §5.4 — one observer per anchored overlay. Bound to the
    /// anchor Widget at present time (`Widget::addObserver`); on the
    /// anchor's detach from the tree it asks the host to dismiss the
    /// overlay it guards. Nested in `Impl` so it can reach the private
    /// erase helpers below.
    class AnchorObserver;

    struct Entry {
        std::uint64_t handleId = 0;
        WidgetPtr widget;
        OverlayTier tier = OverlayTier::Floating;
        OverlayAnchor anchor {};
        OverlayDismissPolicy policy {};
        OverlayOrnamentation ornament {};
        Composition::Rect rect {{0.f, 0.f}, 0.f, 0.f};
        /// O3 §5.4 — the observer wired onto `anchorWidget`, and the
        /// widget it is wired to. Both null unless this overlay was
        /// presented `AtWidget` with `anchorDestroyed == true`. Kept
        /// so the erase paths can un-wire the observer before the
        /// entry dies.
        WidgetObserverPtr anchorObserver;
        Widget * anchorWidget = nullptr;
    };

    /// Set once at construction, never null, never reseated. Pointer
    /// rather than reference so `cppcoreguidelines-avoid-const-or-
    /// ref-data-members` stays quiet — the project lints both ref
    /// members and rule-of-zero violations as a global default.
    WidgetTreeHost * host;
    /// One per tier. Insertion order = present-call order = FIFO
    /// paint order within tier (plan §2). Reverse it for top-first.
    std::array<std::vector<Entry>, kTierCount> entriesByTier {};
    std::uint64_t nextHandleId = 1;

    /// Scratch buffers backing the `ArrayRef`s returned from
    /// `overlaysTopFirst` / `overlaysIn` / `overlaysForPaintIn`.
    /// Rebuilt on each accessor call so callers see current state;
    /// valid until the next accessor or mutating call.
    mutable std::vector<Widget *> topFirstScratch;
    mutable std::array<std::vector<Widget *>, kTierCount> tierScratch {};
    mutable std::array<std::vector<PresentedOverlay>, kTierCount>
        tierPaintScratch {};

    explicit Impl(WidgetTreeHost * h) : host(h) {}

    /// `{0, 0, w, h}` window content bounds for edge-clamping.
    /// Prefers the owning AppWindow's logical size; falls back to the
    /// root widget if the host is detached.
    Composition::Rect windowBounds() const {
        if(host != nullptr && host->ownerWindow_ != nullptr){
            auto r = host->ownerWindow_->getRect();
            return Composition::Rect{{0.f, 0.f}, r.w, r.h};
        }
        if(host != nullptr && host->root != nullptr){
            const auto & rr = host->root->rect();
            return Composition::Rect{{0.f, 0.f}, rr.w, rr.h};
        }
        return Composition::Rect{{0.f, 0.f}, 0.f, 0.f};
    }

    /// Anchor → rect math. `desired` is the overlay widget's own
    /// rect; only its width/height are honored. The returned rect is
    /// edge-clamped against `bounds`. Static because the math reads
    /// only its arguments — Impl is the natural namespace for it but
    /// it does not depend on `host`.
    static Composition::Rect computeRect(const OverlayAnchor & anchor,
                                         const Composition::Rect & desired,
                                         const Composition::Rect & bounds) {
        const float w = desired.w;
        const float h = desired.h;
        float x = 0.f;
        float y = 0.f;

        switch(anchor.mode){
            case OverlayAnchor::Mode::AtWidget: {
                if(anchor.widget == nullptr){
                    // Defensive: fall through to center placement so
                    // a bad anchor still produces a visible overlay
                    // instead of one parked at (0,0).
                    x = bounds.pos.x + ((bounds.w - w) * 0.5f);
                    y = bounds.pos.y + ((bounds.h - h) * 0.5f);
                    break;
                }
                const auto wOrigin = widgetWindowOrigin(anchor.widget);
                const auto & wRect = anchor.widget->rect();
                switch(anchor.edge){
                    case OverlayAnchor::Edge::Bottom:
                        x = wOrigin.x + ((wRect.w - w) * 0.5f);
                        y = wOrigin.y + wRect.h + anchor.gap;
                        break;
                    case OverlayAnchor::Edge::Top:
                        x = wOrigin.x + ((wRect.w - w) * 0.5f);
                        y = wOrigin.y - h - anchor.gap;
                        break;
                    case OverlayAnchor::Edge::Right:
                        x = wOrigin.x + wRect.w + anchor.gap;
                        y = wOrigin.y + ((wRect.h - h) * 0.5f);
                        break;
                    case OverlayAnchor::Edge::Left:
                        x = wOrigin.x - w - anchor.gap;
                        y = wOrigin.y + ((wRect.h - h) * 0.5f);
                        break;
                }
                break;
            }
            case OverlayAnchor::Mode::AtPoint:
                x = anchor.point.x;
                y = anchor.point.y;
                break;
            case OverlayAnchor::Mode::CenterInWindow:
                x = bounds.pos.x + ((bounds.w - w) * 0.5f);
                y = bounds.pos.y + ((bounds.h - h) * 0.5f);
                break;
        }

        // Edge-clamp. If the overlay is larger than the window in a
        // given axis, max-clamp collapses to bounds.pos so the
        // overlay's top-left stays visible (the overflow falls off
        // the bottom/right). The min-clamp prevents negative origins.
        const float maxX = bounds.pos.x + std::max(0.f, bounds.w - w);
        const float maxY = bounds.pos.y + std::max(0.f, bounds.h - h);
        x = std::max(bounds.pos.x, std::min(x, maxX));
        y = std::max(bounds.pos.y, std::min(y, maxY));

        return Composition::Rect{Composition::Point2D{x, y}, w, h};
    }

    Entry * findById(std::uint64_t id){
        if(id == 0){
            return nullptr;
        }
        for(auto & bucket : entriesByTier){
            for(auto & entry : bucket){
                if(entry.handleId == id){
                    return &entry;
                }
            }
        }
        return nullptr;
    }

    const Entry * findById(std::uint64_t id) const {
        return const_cast<Impl *>(this)->findById(id);
    }

    /// O3 §5.4 — un-wire an entry's anchor observer from its anchor
    /// widget before the entry is erased. `skipWidget` is the widget
    /// currently mid-detach-notify (its `notifyObservers` loop is on
    /// the stack): removing an observer from it now would mutate the
    /// vector being iterated, so that one is left for the widget's own
    /// teardown. All non-detach callers pass nullptr and un-wire
    /// eagerly so a still-living anchor does not keep a dead observer.
    void detachAnchorObserver(Entry & e, Widget * skipWidget){
        if(e.anchorObserver != nullptr && e.anchorWidget != nullptr &&
           e.anchorWidget != skipWidget){
            e.anchorWidget->removeObserver(e.anchorObserver);
        }
        e.anchorObserver = nullptr;
        e.anchorWidget = nullptr;
    }

    /// Full teardown of an entry about to be erased: un-wire its anchor
    /// observer (see above) and un-thread the host from the overlay's
    /// subtree (the inverse of the `mountOverlay` in `present`). Every
    /// erase path funnels through this so a dismissed overlay is left
    /// detached, not a dangling host-wired subtree.
    void teardownEntry(Entry & e, Widget * skipDetachWidget){
        detachAnchorObserver(e, skipDetachWidget);
        if(host != nullptr && e.widget != nullptr){
            host->unmountOverlay(e.widget.get());
        }
    }

    /// A dismissed overlay must vanish from the composite: force a
    /// clean main-tree repaint (so the region the overlay covered is
    /// redrawn) and ask the window for a frame. Without the root
    /// mark, `FrameBuilder::buildFrame` would early-return on a clean
    /// main tree once the last overlay is gone and the deposited frame
    /// would still show the stale overlay pixels. Mirror of the
    /// present-time `requestFrame` in `OverlayHost::present`.
    void requestDismissRepaint(){
        if(host == nullptr){
            return;
        }
        // Public `viewRef()` (not the protected `view` field) — O1
        // kept Widget's coupling surface unchanged and this follows
        // suit. Root is guaranteed to have a view (Widget always
        // constructs one), so the reference is safe once root is set.
        if(host->root != nullptr){
            host->root->viewRef().markDirty(View::Paint);
        }
        host->requestFrame();
    }

    /// Erase the entry with `handleId`, un-wiring its anchor observer
    /// first (skipping `skipDetachWidget` when the call originates from
    /// that widget's own detach notification). Returns true if an entry
    /// was removed. Repaint is requested by the caller once, after a
    /// batch, so a multi-overlay dismissal coalesces into one frame.
    bool eraseById(std::uint64_t id, Widget * skipDetachWidget = nullptr){
        if(id == 0){
            return false;
        }
        for(auto & bucket : entriesByTier){
            auto it = std::find_if(bucket.begin(), bucket.end(),
                                   [&](const Entry & e){
                                       return e.handleId == id;
                                   });
            if(it != bucket.end()){
                teardownEntry(*it, skipDetachWidget);
                bucket.erase(it);
                return true;
            }
        }
        return false;
    }
};

/// O3 §5.4 — defined out-of-line now that `Impl` is complete.
class OverlayHost::Impl::AnchorObserver : public WidgetObserver {
    /// Non-owning back-pointer to the host impl. The host outlives
    /// every overlay it presents (the observer is un-wired on dismiss
    /// / detach before either dies), so this never dangles.
    Impl * impl_;
    std::uint64_t handleId_;
public:
    AnchorObserver(Impl * impl, std::uint64_t handleId)
        : impl_(impl), handleId_(handleId) {}

    void onWidgetDetach(WidgetPtr) override {
        // We are running inside the anchor widget's `notifyObservers`
        // loop, so this entry's `anchorWidget` is exactly the widget
        // being iterated — pass it as `skipDetachWidget` so
        // `detachAnchorObserver` does not `removeObserver` it mid-loop.
        if(const Impl::Entry * e = impl_->findById(handleId_)){
            Widget * anchor = e->anchorWidget;
            if(impl_->eraseById(handleId_, anchor)){
                impl_->requestDismissRepaint();
            }
        }
    }
};

OverlayHost::OverlayHost(WidgetTreeHost & host)
    : impl_(Core::UniquePtr<Impl>(new Impl(&host))) {}

OverlayHost::~OverlayHost() = default;

OverlayHandle OverlayHost::present(WidgetPtr overlay,
                                   OverlayTier tier,
                                   const OverlayAnchor & anchor,
                                   const OverlayDismissPolicy & policy,
                                   const OverlayOrnamentation & ornament){
    if(overlay == nullptr){
        return OverlayHandle{};
    }

    const auto desired = overlay->rect();
    const auto bounds = impl_->windowBounds();
    const auto resolved = impl_->computeRect(anchor, desired, bounds);

    // Commit the resolved rect to the overlay widget. The widget is
    // not in the main tree, so `setRect` will not propagate to a
    // parent's `onChildRectCommitted` — it only resizes the widget's
    // own view, which is what we want here.
    overlay->setRect(resolved);

    // Make the overlay renderable: propagate the window render target
    // and thread the host down its subtree (the overlay lives outside
    // the main tree, so the `initWidgetTree` wiring never ran for it).
    // Must precede the markDirty below so the first `buildFrame` sees a
    // fully-wired view. O1/O2 shipped the paint walk but left this gap;
    // it is closed here because T1 (tooltip) is the first overlay that
    // actually renders.
    if(impl_->host != nullptr){
        impl_->host->mountOverlay(overlay.get());
    }

    // O2: mark the overlay's root view dirty across Style / Layout /
    // Paint so the first `FrameBuilder::buildFrame(*overlay->view)`
    // call runs all three passes. `Widget::setRect` only flows
    // through `View::resize`, which updates the rect and emits
    // `onLayoutResolved` but does NOT mark dirty (View.Core.cpp:194).
    // Without this, the overlay's first paint silently no-ops because
    // its root mask is zero. Subsequent paintDirty passes mark Paint
    // only (see `WidgetTreeHost::paintDirty`) to avoid spurious
    // Layout/Style work on unchanged overlays.
    overlay->viewRef().markDirty(
        View::Style | View::Layout | View::Paint);

    // O2: request a frame so the overlay shows on the next vsync.
    // Overlay widgets sit outside the main tree, so `Widget::invalidate`
    // (the usual driver of `WidgetTreeHost::requestFrame`) never gets
    // a chance to run — `setRect` short-circuits because
    // `treeHost == nullptr && impl_->hasMounted == false` on the
    // presented widget. We route the request through the host
    // directly. If the host's owner window is null (detached host),
    // `requestFrame` is itself a no-op.
    if(impl_->host != nullptr){
        impl_->host->requestFrame();
    }

    Impl::Entry entry;
    entry.handleId = impl_->nextHandleId++;
    entry.widget = overlay;
    entry.tier = tier;
    entry.anchor = anchor;
    entry.policy = policy;
    entry.ornament = ornament;
    entry.rect = resolved;

    // O3 §5.4 — anchor-destruction dismissal. When the overlay is
    // anchored to a Widget and its policy opts in, wire an observer
    // onto that anchor so the overlay dismisses if the anchor leaves
    // the tree (e.g. a popover must not outlive the button that opened
    // it). Only meaningful for `AtWidget`; the other anchor modes have
    // no widget to observe.
    if(policy.anchorDestroyed && anchor.mode == OverlayAnchor::Mode::AtWidget &&
       anchor.widget != nullptr){
        entry.anchorWidget = anchor.widget;
        entry.anchorObserver = std::make_shared<Impl::AnchorObserver>(
            impl_.get(), entry.handleId);
        anchor.widget->addObserver(entry.anchorObserver);
    }

    impl_->entriesByTier[tierIndex(tier)].push_back(std::move(entry));

    return OverlayHandle{impl_->entriesByTier[tierIndex(tier)].back().handleId};
}

void OverlayHost::dismiss(OverlayHandle handle){
    if(!handle.valid()){
        return;
    }
    if(impl_->eraseById(handle.id)){
        impl_->requestDismissRepaint();
    }
}

void OverlayHost::dismiss(Widget * overlay){
    if(overlay == nullptr){
        return;
    }
    bool removed = false;
    for(auto & bucket : impl_->entriesByTier){
        for(auto it = bucket.begin(); it != bucket.end(); ){
            if(it->widget.get() == overlay){
                impl_->teardownEntry(*it, nullptr);
                it = bucket.erase(it);
                removed = true;
            }
            else {
                ++it;
            }
        }
    }
    if(removed){
        impl_->requestDismissRepaint();
    }
}

void OverlayHost::dismissAll(OverlayTier tier){
    auto & bucket = impl_->entriesByTier[tierIndex(tier)];
    if(bucket.empty()){
        return;
    }
    for(auto & entry : bucket){
        impl_->teardownEntry(entry, nullptr);
    }
    bucket.clear();
    impl_->requestDismissRepaint();
}

void OverlayHost::dismissAll(){
    bool removed = false;
    for(auto & bucket : impl_->entriesByTier){
        for(auto & entry : bucket){
            impl_->teardownEntry(entry, nullptr);
            removed = true;
        }
        bucket.clear();
    }
    if(removed){
        impl_->requestDismissRepaint();
    }
}

bool OverlayHost::isPresenting(OverlayTier tier) const {
    return !impl_->entriesByTier[tierIndex(tier)].empty();
}

bool OverlayHost::isPresentingAny() const {
    for(const auto & bucket : impl_->entriesByTier){
        if(!bucket.empty()){
            return true;
        }
    }
    return false;
}

OmegaCommon::ArrayRef<Widget *> OverlayHost::overlaysTopFirst() const {
    auto & scratch = impl_->topFirstScratch;
    scratch.clear();
    // Reverse tier order (highest tier paints last, hits first), then
    // reverse insertion order within tier (most recently presented
    // overlay sits on top of older siblings in the same tier).
    for(std::size_t i = kTierCount; i > 0; --i){
        const auto & bucket = impl_->entriesByTier[i - 1];
        for(auto it = bucket.rbegin(); it != bucket.rend(); ++it){
            scratch.push_back(it->widget.get());
        }
    }
    return OmegaCommon::ArrayRef<Widget *>(scratch);
}

OmegaCommon::ArrayRef<Widget *> OverlayHost::overlaysIn(OverlayTier tier) const {
    const auto idx = tierIndex(tier);
    auto & scratch = impl_->tierScratch[idx];
    scratch.clear();
    for(const auto & entry : impl_->entriesByTier[idx]){
        scratch.push_back(entry.widget.get());
    }
    return OmegaCommon::ArrayRef<Widget *>(scratch);
}

OmegaCommon::ArrayRef<PresentedOverlay> OverlayHost::overlaysForPaintIn(
        OverlayTier tier) const {
    const auto idx = tierIndex(tier);
    auto & scratch = impl_->tierPaintScratch[idx];
    scratch.clear();
    for(const auto & entry : impl_->entriesByTier[idx]){
        PresentedOverlay po;
        po.widget = entry.widget.get();
        po.rect = entry.rect;
        po.ornament = entry.ornament;
        scratch.push_back(std::move(po));
    }
    return OmegaCommon::ArrayRef<PresentedOverlay>(scratch);
}

Widget * OverlayHost::absorbingOverlayAt(
        const Composition::Point2D & point) const {
    // Top-first: reverse tier order (DragGhost → Tooltip → Modal →
    // Floating), then reverse insertion order within tier. The first
    // overlay that contains the point *and* absorbs hits claims it; a
    // containing-but-transparent overlay (Tooltip / DragGhost) is
    // skipped so the walk falls through to lower overlays and the
    // main tree (plan §5.1 — "you can click through a tooltip").
    for(std::size_t i = kTierCount; i > 0; --i){
        const auto & bucket = impl_->entriesByTier[i - 1];
        for(auto it = bucket.rbegin(); it != bucket.rend(); ++it){
            if(!rectContainsPoint(it->rect, point)){
                continue;
            }
            if(it->policy.absorbsHits){
                return it->widget.get();
            }
        }
    }
    return nullptr;
}

bool OverlayHost::dismissClickOutside(const Composition::Point2D & point){
    // Gather the handles to dismiss top-first, then erase in a second
    // pass so a bucket is never mutated while it is being walked.
    std::vector<std::uint64_t> toDismiss;
    for(std::size_t i = kTierCount; i > 0; --i){
        const auto & bucket = impl_->entriesByTier[i - 1];
        for(auto it = bucket.rbegin(); it != bucket.rend(); ++it){
            if(it->policy.clickOutside && !rectContainsPoint(it->rect, point)){
                toDismiss.push_back(it->handleId);
            }
        }
    }
    bool removed = false;
    for(auto id : toDismiss){
        removed = impl_->eraseById(id) || removed;
    }
    if(removed){
        impl_->requestDismissRepaint();
    }
    return removed;
}

bool OverlayHost::dismissTopmostForEscape(){
    for(std::size_t i = kTierCount; i > 0; --i){
        auto & bucket = impl_->entriesByTier[i - 1];
        for(auto it = bucket.rbegin(); it != bucket.rend(); ++it){
            if(it->policy.escapeKey){
                if(impl_->eraseById(it->handleId)){
                    impl_->requestDismissRepaint();
                }
                return true;
            }
        }
    }
    return false;
}

Composition::Rect OverlayHost::rectFor(OverlayHandle handle) const {
    if(const auto * entry = impl_->findById(handle.id)){
        return entry->rect;
    }
    return Composition::Rect{{0.f, 0.f}, 0.f, 0.f};
}

OverlayTier OverlayHost::tierFor(OverlayHandle handle) const {
    if(const auto * entry = impl_->findById(handle.id)){
        return entry->tier;
    }
    return OverlayTier::Floating;
}

void OverlayHost::relayoutAll(){
    const auto bounds = impl_->windowBounds();
    for(auto & bucket : impl_->entriesByTier){
        for(auto & entry : bucket){
            if(entry.widget == nullptr){
                continue;
            }
            const auto desired = entry.widget->rect();
            entry.rect = impl_->computeRect(entry.anchor, desired, bounds);
            entry.widget->setRect(entry.rect);
        }
    }
}

}
