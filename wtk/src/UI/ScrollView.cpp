#include "omegaWTK/UI/ScrollView.h"
#include "omegaWTK/UI/LayoutManager.h"
#include "omegaWTK/Composition/DisplayList.h"
#include "omegaWTK/Composition/Brush.h"

#include <algorithm>
#include <cstdio>

namespace OmegaWTK {

    namespace {
        constexpr float kScrollBarThickness = 6.f;
        constexpr float kScrollBarMargin = 2.f;
        constexpr float kScrollBarRadius = 3.f;
        constexpr float kMinThumbLength = 20.f;
        // Per-op fill carried directly on the `RoundedRect` DrawOp; the
        // pre-3.6 grey is preserved for visual parity.
        constexpr Composition::Color kScrollBarColor {0.5f, 0.5f, 0.5f, 0.6f};

        // V5: per-keypress arrow-key scroll distance. PageUp/PageDown use
        // the viewport extent; Home/End jump to the ends.
        constexpr float kKeyScrollStep = 40.f;

        // V2.1 delivery trace, gated on OMEGAWTK_SCROLL_TRACE (any value
        // not starting with '0'). Lets the developer confirm a wheel
        // event actually reached a ScrollView's handler and whether it
        // was consumed or bubbled.
        bool scrollTraceEnabled(){
            static int cached = -1;
            if(cached >= 0){
                return cached == 1;
            }
            auto envVar = OmegaCommon::getEnvVar("OMEGAWTK_SCROLL_TRACE");
            cached = (envVar.has_value() && !envVar->empty()
                      && envVar->front() != '0') ? 1 : 0;
            return cached == 1;
        }
    }

    void ScrollView::DefaultScrollHandler::onRecieveEvent(Native::NativeEventPtr event){
        if(event == nullptr || owner == nullptr){
            return;
        }
        const bool isWheel = event->type == Native::NativeEvent::ScrollWheel;
        const bool isKey   = event->type == Native::NativeEvent::KeyDown;
        if(!isWheel && !isKey){
            return;
        }

        // Scroll range per axis (content extent minus viewport). A zero
        // range means this axis cannot scroll here.
        float maxX = 0.f;
        float maxY = 0.f;
        if(owner->child != nullptr){
            const auto & content = owner->child->getRect();
            const auto & viewport = owner->getRect();
            maxX = std::max(0.f, content.w - viewport.w);
            maxY = std::max(0.f, content.h - viewport.h);
        }

        Composition::Point2D newOffset = owner->scrollOffset;
        bool consumed = false;

        if(isWheel){
            auto *p = static_cast<Native::ScrollParams *>(event->params);
            if(p == nullptr){
                return;
            }
            // Invariant B (axis-aware consumption): consume the wheel only
            // on an axis this ScrollView actually scrolls — enabled AND has
            // range AND the wheel moved on that axis. An axis we do not
            // scroll is left unconsumed so the event bubbles to an ancestor
            // that might (e.g. an inner horizontal list inside a vertical
            // page). "Has range" (not "not at the end") is the gate: a
            // ScrollView at its limit still consumes, so v0 does not chain
            // at the limit (matches NSScrollView). The clamp below pins the
            // offset to the end.
            if(owner->hasHorizontalScrollBar && maxX > 0.f && p->deltaX != 0.f){
                newOffset.x -= p->deltaX;
                consumed = true;
            }
            if(owner->hasVerticalScrollBar && maxY > 0.f && p->deltaY != 0.f){
                newOffset.y -= p->deltaY;
                consumed = true;
            }
            if(!consumed){
                // Nothing this ScrollView handles — leave `event->handled`
                // false so `View::dispatchEvent` bubbles it to an ancestor.
                if(scrollTraceEnabled()){
                    std::fprintf(stderr,
                        "[OmegaWTKScroll] sv=%p wheel delta={%.2f,%.2f} range={%.1f,%.1f} -> bubble\n",
                        static_cast<const void *>(owner),
                        p->deltaX, p->deltaY, maxX, maxY);
                }
                return;
            }
        }
        else { // isKey — V5 keyboard scroll (this ScrollView holds focus).
            auto *kp = static_cast<Native::KeyDownParams *>(event->params);
            if(kp == nullptr){
                return;
            }
            const float pageV = owner->getRect().h;
            if(owner->hasVerticalScrollBar && maxY > 0.f){
                switch(kp->code){
                    case Native::KeyCode::ArrowUp:   newOffset.y -= kKeyScrollStep; consumed = true; break;
                    case Native::KeyCode::ArrowDown: newOffset.y += kKeyScrollStep; consumed = true; break;
                    case Native::KeyCode::PageUp:    newOffset.y -= pageV;          consumed = true; break;
                    case Native::KeyCode::PageDown:  newOffset.y += pageV;          consumed = true; break;
                    case Native::KeyCode::Home:      newOffset.y = 0.f;             consumed = true; break;
                    case Native::KeyCode::End:       newOffset.y = maxY;            consumed = true; break;
                    default: break;
                }
            }
            if(!consumed && owner->hasHorizontalScrollBar && maxX > 0.f){
                switch(kp->code){
                    case Native::KeyCode::ArrowLeft:  newOffset.x -= kKeyScrollStep; consumed = true; break;
                    case Native::KeyCode::ArrowRight: newOffset.x += kKeyScrollStep; consumed = true; break;
                    default: break;
                }
            }
            if(!consumed){
                // A key this ScrollView does not scroll on. Keyboard events
                // route to the focused view (no bubbling today), so just
                // leave it unhandled.
                return;
            }
        }

        newOffset.x = std::clamp(newOffset.x, 0.f, maxX);
        newOffset.y = std::clamp(newOffset.y, 0.f, maxY);
        owner->setScrollOffset(newOffset);
        event->handled = true;

        if(scrollTraceEnabled()){
            std::fprintf(stderr,
                "[OmegaWTKScroll] sv=%p %s range={%.1f,%.1f} -> offset={%.1f,%.1f}\n",
                static_cast<const void *>(owner),
                isWheel ? "wheel" : "key",
                maxX, maxY, newOffset.x, newOffset.y);
        }
    }

    ScrollView::ScrollView(const Composition::Rect & rect, SharedHandle<View> child, bool hasVerticalScrollBar, bool hasHorizontalScrollBar, ViewPtr parent):
            View(rect, parent),
            child(child),
            hasVerticalScrollBar(hasVerticalScrollBar),
            hasHorizontalScrollBar(hasHorizontalScrollBar){
        defaultHandler.owner = this;
        setReciever(&defaultHandler);
        // V5: make the viewport focusable so the FocusManager can route
        // keyboard scroll keys here. StrongFocus = ClickFocus | TabFocus:
        // clicking inside the scrolled content focuses the nearest
        // click-focusable ancestor (the M1 walk in WidgetTreeHost), and
        // Tab traversal can land on it. A focused ScrollView then handles
        // PageUp/Down/Home/End/arrows in DefaultScrollHandler.
        setFocusPolicy(FocusPolicy::StrongFocus);
        // V1: a scroll viewport must NOT relayout its content child — the
        // content extent is owned by the host and is deliberately larger
        // than the viewport. Without this, the default AbsoluteLayout
        // FitContent-clamps the child down to the viewport and the scroll
        // range collapses (ScrollView-4.7-Integration-Plan §0 symptom #1).
        setLayoutManager(&PassthroughLayout::instance());
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
        const bool changed = offset.x != scrollOffset.x
                          || offset.y != scrollOffset.y;
        scrollOffset = offset;
        // The next paint pass picks up the new offset through
        // `contentOffset()`, folded into the FrameBuilder paint walker's
        // accumulator for every descendant. Tier 3 Phase 3.6 left
        // scheduling that paint to the caller; V2.1 (ScrollView-4.7-
        // Integration-Plan) makes setScrollOffset self-sufficient —
        // a scroll-wheel handler runs inside native-event dispatch where
        // nothing else requests the frame, so without this the offset
        // changes but the screen never updates. Only on an actual change
        // (a no-op set, e.g. an already-clamped offset, should not dirty
        // the tree). `scheduleRepaint` is a no-op before the view is
        // attached to a host, so construction-time clamps (e.g.
        // ScrollableContainer::setContentSize) stay cheap.
        if(changed){
            scheduleRepaint();
        }
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

    void ScrollView::paint(Composition::PaintContext & pc){
        // V3: open the content clip. `pc.offset` is this view's absolute
        // window offset (the 4.7 paint walker bakes it into every emitted
        // op), so the clip rect is the viewport in absolute window coords.
        // The matching PopClip is emitted by `paintAfterChildren` once all
        // descendants have painted; the walker calls the two around the
        // child subtree so the scissor brackets exactly the content.
        const auto & r = getRect();
        Composition::Rect windowClip{
            Composition::Point2D{pc.offset.x, pc.offset.y}, r.w, r.h};
        pc.displayList.append(Composition::DrawOp::makePushClip(windowClip));
    }

    void ScrollView::paintAfterChildren(Composition::PaintContext & pc){
        // V3: close the content clip opened in `paint`.
        pc.displayList.append(Composition::DrawOp::makePopClip());

        // V4: emit the scroll-bar thumbs AFTER the PopClip so they draw
        // outside the viewport scissor. Positions are baked into absolute
        // window coords with `pc.offset` (the 4.7 walker no longer applies
        // a per-view replay translation). A bar is drawn only when its axis
        // actually overflows — a non-scrolling axis shows no bar.
        if(child == nullptr){
            return;
        }
        const auto & viewRect = getRect();
        const auto & contentRect = child->getRect();
        const float ox = pc.offset.x;
        const float oy = pc.offset.y;

        if(hasVerticalScrollBar && contentRect.h > viewRect.h){
            const float trackH = viewRect.h - 2.f * kScrollBarMargin;
            const float thumbRatio = viewRect.h / contentRect.h;
            const float thumbH = std::max(kMinThumbLength, trackH * thumbRatio);
            float scrollRatio = scrollOffset.y / (contentRect.h - viewRect.h);
            scrollRatio = std::clamp(scrollRatio, 0.f, 1.f);
            const float thumbY = scrollRatio * (trackH - thumbH);

            Composition::RoundedRect thumb {
                {ox + viewRect.w - kScrollBarThickness - kScrollBarMargin,
                 oy + kScrollBarMargin + thumbY},
                kScrollBarThickness, thumbH,
                kScrollBarRadius, kScrollBarRadius
            };
            pc.displayList.append(Composition::DrawOp{
                thumb, Composition::ColorBrush(kScrollBarColor)});
        }

        if(hasHorizontalScrollBar && contentRect.w > viewRect.w){
            const float trackW = viewRect.w - 2.f * kScrollBarMargin;
            const float thumbRatio = viewRect.w / contentRect.w;
            const float thumbW = std::max(kMinThumbLength, trackW * thumbRatio);
            float scrollRatio = scrollOffset.x / (contentRect.w - viewRect.w);
            scrollRatio = std::clamp(scrollRatio, 0.f, 1.f);
            const float thumbX = scrollRatio * (trackW - thumbW);

            Composition::RoundedRect thumb {
                {ox + kScrollBarMargin + thumbX,
                 oy + viewRect.h - kScrollBarThickness - kScrollBarMargin},
                thumbW, kScrollBarThickness,
                kScrollBarRadius, kScrollBarRadius
            };
            pc.displayList.append(Composition::DrawOp{
                thumb, Composition::ColorBrush(kScrollBarColor)});
        }
    }

    bool ScrollView::clipsContentSubtree() const{
        return true;
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
