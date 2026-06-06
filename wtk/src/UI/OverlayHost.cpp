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

} // namespace

struct OverlayHost::Impl {
    struct Entry {
        std::uint64_t handleId = 0;
        WidgetPtr widget;
        OverlayTier tier = OverlayTier::Floating;
        OverlayAnchor anchor {};
        OverlayDismissPolicy policy {};
        OverlayOrnamentation ornament {};
        Composition::Rect rect {{0.f, 0.f}, 0.f, 0.f};
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
    /// `overlaysTopFirst` / `overlaysIn`. Rebuilt on each accessor
    /// call so callers see current state; valid until the next
    /// accessor or mutating call.
    mutable std::vector<Widget *> topFirstScratch;
    mutable std::array<std::vector<Widget *>, kTierCount> tierScratch {};

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

    Impl::Entry entry;
    entry.handleId = impl_->nextHandleId++;
    entry.widget = overlay;
    entry.tier = tier;
    entry.anchor = anchor;
    entry.policy = policy;
    entry.ornament = ornament;
    entry.rect = resolved;

    impl_->entriesByTier[tierIndex(tier)].push_back(std::move(entry));

    return OverlayHandle{impl_->entriesByTier[tierIndex(tier)].back().handleId};
}

void OverlayHost::dismiss(OverlayHandle handle){
    if(!handle.valid()){
        return;
    }
    for(auto & bucket : impl_->entriesByTier){
        auto it = std::find_if(bucket.begin(), bucket.end(),
                               [&](const Impl::Entry & e){
                                   return e.handleId == handle.id;
                               });
        if(it != bucket.end()){
            bucket.erase(it);
            return;
        }
    }
}

void OverlayHost::dismiss(Widget * overlay){
    if(overlay == nullptr){
        return;
    }
    for(auto & bucket : impl_->entriesByTier){
        bucket.erase(std::remove_if(bucket.begin(), bucket.end(),
                                    [&](const Impl::Entry & e){
                                        return e.widget.get() == overlay;
                                    }),
                     bucket.end());
    }
}

void OverlayHost::dismissAll(OverlayTier tier){
    impl_->entriesByTier[tierIndex(tier)].clear();
}

void OverlayHost::dismissAll(){
    for(auto & bucket : impl_->entriesByTier){
        bucket.clear();
    }
}

bool OverlayHost::isPresenting(OverlayTier tier) const {
    return !impl_->entriesByTier[tierIndex(tier)].empty();
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
