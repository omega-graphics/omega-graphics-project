#include "omegaWTK/UI/ScrollView.h"
#include "omegaWTK/Composition/DisplayList.h"
#include "omegaWTK/Composition/Brush.h"

#include <algorithm>

namespace OmegaWTK {

    namespace {
        constexpr float kScrollBarThickness = 6.f;
        constexpr float kScrollBarMargin = 2.f;
        constexpr float kScrollBarRadius = 3.f;
        constexpr float kMinThumbLength = 20.f;
        // Tier 3 Phase 3.6: bar fill is no longer mediated by a brush
        // applied per-canvas — it is a per-op color carried directly on
        // the `RoundedRect` DrawOp. The pre-3.6 grey (0.5, 0.5, 0.5,
        // 0.6) is preserved for visual parity with the deleted
        // overlay-layer path.
        constexpr Composition::Color kScrollBarColor {0.5f, 0.5f, 0.5f, 0.6f};
    }

    void ScrollView::DefaultScrollHandler::onRecieveEvent(Native::NativeEventPtr event){
        if(event == nullptr || owner == nullptr){
            return;
        }
        if(event->type == Native::NativeEvent::ScrollWheel){
            auto *p = static_cast<Native::ScrollParams *>(event->params);
            if(p != nullptr){
                Composition::Point2D newOffset = owner->scrollOffset;
                if(owner->hasHorizontalScrollBar){
                    newOffset.x -= p->deltaX;
                }
                if(owner->hasVerticalScrollBar){
                    newOffset.y -= p->deltaY;
                }
                newOffset.x = std::max(0.f, newOffset.x);
                newOffset.y = std::max(0.f, newOffset.y);
                owner->setScrollOffset(newOffset);
            }
        }
    }

    ScrollView::ScrollView(const Composition::Rect & rect, SharedHandle<View> child, bool hasVerticalScrollBar, bool hasHorizontalScrollBar, ViewPtr parent):
            View(rect, parent),
            child(child),
            hasVerticalScrollBar(hasVerticalScrollBar),
            hasHorizontalScrollBar(hasHorizontalScrollBar){
        defaultHandler.owner = this;
        setReciever(&defaultHandler);
        if(child != nullptr){
            addSubView(child.get());
        }
        // Tier 3 Phase 3.6: the prior per-view overlay layers and
        // canvases (vScrollBarLayer / hScrollBarLayer /
        // vScrollBarCanvas / hScrollBarCanvas) are gone — bars are
        // authored declaratively and emitted as `RoundedRect` DrawOps
        // from `paintOverlay()` into the shared window canvas.
    }

    bool ScrollView::hasDelegate(){
        return delegate != nullptr;
    };

    void ScrollView::setDelegate(ScrollViewDelegate *_delegate){
        if(_delegate == nullptr){
            delegate = nullptr;
            setReciever(&defaultHandler);
            return;
        }
        delegate = _delegate;
        delegate->scrollView = this;
        setReciever(delegate);
    };

    void ScrollView::setScrollOffset(const Composition::Point2D & offset){
        scrollOffset = offset;
        // Tier 3 Phase 3.6: previously called `paintScrollBars()`
        // which immediately re-rendered the overlay-layer canvases.
        // Those layers are gone — the next paint pass picks up the
        // new offset through `paint()` / `paintOverlay()` (descendants
        // see the shift via `contentOffset()` folded into the
        // FrameBuilder accumulator). The compositor invalidation that
        // schedules the next paint is the caller's responsibility
        // (typically the input handler triggers a widget invalidate).
    }

    Composition::Point2D ScrollView::contentOffset() const{
        // Sign: descendant `legacyComputeWindowOffset()` adds this
        // (Phase 3.6 walk), so returning `-scrollOffset` shifts the
        // content up/left by `scrollOffset` — same observable effect
        // as the pre-3.6 `scrollOffsetContribution() = +scrollOffset`
        // path, which was *subtracted*.
        return {-scrollOffset.x, -scrollOffset.y};
    }

    bool ScrollView::wantsLayer() const{
        return true;
    }

    void ScrollView::paint(Composition::DisplayList & list) const{
        // The clip rect is the ScrollView's own bounds expressed in
        // its local coordinate space (origin {0,0} relative to itself).
        // FrameBuilder's per-view replay translates by the view's
        // window-offset at sendFrame time, so `PushClip` lands at the
        // ScrollView's absolute window rect on the shared canvas.
        const auto & r = const_cast<ScrollView *>(this)->getRect();
        Composition::Rect localClip{
            Composition::Point2D{0.f, 0.f}, r.w, r.h};
        list.append(Composition::DrawOp::makePushClip(localClip));
    }

    void ScrollView::paintOverlay(Composition::DisplayList & list) const{
        list.append(Composition::DrawOp::makePopClip());

        const auto & viewRect = const_cast<ScrollView *>(this)->getRect();
        if(child == nullptr){
            return;
        }
        const auto & contentRect = child->getRect();

        if(hasVerticalScrollBar){
            float trackH = viewRect.h - 2.f * kScrollBarMargin;
            float contentH = contentRect.h;
            // Avoid divide-by-zero / negative ratios when content is
            // smaller than the viewport. We still draw the thumb
            // (full-track) in that case so the bar is visible.
            if(contentH <= viewRect.h){
                contentH = viewRect.h + 1.f;
            }
            float thumbRatio = viewRect.h / contentH;
            float thumbH = std::max(kMinThumbLength, trackH * thumbRatio);
            float scrollRatio = scrollOffset.y / (contentH - viewRect.h);
            scrollRatio = std::clamp(scrollRatio, 0.f, 1.f);
            float thumbY = scrollRatio * (trackH - thumbH);

            Composition::RoundedRect thumb {
                {viewRect.w - kScrollBarThickness - kScrollBarMargin,
                 kScrollBarMargin + thumbY},
                kScrollBarThickness, thumbH,
                kScrollBarRadius, kScrollBarRadius
            };
            list.append(Composition::DrawOp{
                thumb, Composition::ColorBrush(kScrollBarColor)});
        }

        if(hasHorizontalScrollBar){
            float trackW = viewRect.w - 2.f * kScrollBarMargin;
            float contentW = contentRect.w;
            if(contentW <= viewRect.w){
                contentW = viewRect.w + 1.f;
            }
            float thumbRatio = viewRect.w / contentW;
            float thumbW = std::max(kMinThumbLength, trackW * thumbRatio);
            float scrollRatio = scrollOffset.x / (contentW - viewRect.w);
            scrollRatio = std::clamp(scrollRatio, 0.f, 1.f);
            float thumbX = scrollRatio * (trackW - thumbW);

            Composition::RoundedRect thumb {
                {kScrollBarMargin + thumbX,
                 viewRect.h - kScrollBarThickness - kScrollBarMargin},
                thumbW, kScrollBarThickness,
                kScrollBarRadius, kScrollBarRadius
            };
            list.append(Composition::DrawOp{
                thumb, Composition::ColorBrush(kScrollBarColor)});
        }
    }

    void ScrollViewDelegate::onRecieveEvent(Native::NativeEventPtr event){
        if(event == nullptr){
            return;
        }
        switch(event->type){
            case Native::NativeEvent::ScrollWheel: {
                auto *p = static_cast<Native::ScrollParams *>(event->params);
                if(p != nullptr){
                    onScrollWheel(p->deltaX, p->deltaY);
                }
                break;
            }
            default:
                break;
        }
    };

};
