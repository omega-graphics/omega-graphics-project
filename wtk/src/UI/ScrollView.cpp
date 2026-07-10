#include "omegaWTK/UI/ScrollView.h"
#include "omegaWTK/UI/LayoutManager.h"
#include "omegaWTK/Composition/DisplayList.h"
#include "omegaWTK/Composition/Brush.h"
// E4: reach the per-window AnimationScheduler for fling momentum. These are
// UI-private headers (same dir); ScrollView is a friend of View so it may
// read `impl_->treeHost_`.
#include "ViewImpl.h"
#include "WidgetTreeHost.h"
#include "FrameBuilder.h"
#include "AnimationScheduler.h"

#include <algorithm>
#include <cmath>
#include <chrono>
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

        // E4 fling-momentum tuning.
        constexpr float kMinFlingSpeed   = 120.f;  // px/sec release threshold
        constexpr float kFlingProjectSec = 0.22f;  // how far velocity carries
        constexpr float kFlingMsPerPx    = 2.2f;   // tween duration ∝ distance
        constexpr float kFlingMinMs      = 220.f;
        constexpr float kFlingMaxMs      = 1100.f;

        double nowSeconds(){
            return std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        }

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
        // E3: scroll-bar drag lives in its own method (press/move/release).
        if(event->type == Native::NativeEvent::LMouseDown
           || event->type == Native::NativeEvent::LMouseUp
           || event->type == Native::NativeEvent::CursorMove){
            owner->handleDragPointer(event);
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

        const Composition::Point2D prevOffset = owner->scrollOffset;
        Composition::Point2D newOffset = owner->scrollOffset;
        bool consumed = false;
        Native::ScrollPhase wheelPhase = Native::ScrollPhase::None;

        if(isWheel){
            auto *p = static_cast<Native::ScrollParams *>(event->params);
            if(p == nullptr){
                return;
            }
            wheelPhase = p->phase;
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
        // E4/E5: fresh user input cancels the in-flight glide so the action
        // wins; a discrete mouse wheel then re-arms its own momentum below.
        owner->cancelFling();
        owner->setScrollOffset(newOffset);
        event->handled = true;

        // E5: momentum. A discrete mouse wheel (`phase == None`) carries no
        // OS inertia, so synthesize an app-side fling from the per-tick
        // velocity; rapid ticks re-project it further and, once ticks stop,
        // the last fling coasts to rest. A TRACKPAD (any real phase) already
        // streams its own decaying momentum deltas from the OS, so we simply
        // apply those and never add app momentum on top (no double-glide).
        if(isWheel && wheelPhase == Native::ScrollPhase::None){
            const double now = nowSeconds();
            const double dt = now - owner->wheelLastTimeSec_;
            owner->wheelLastTimeSec_ = now;
            const float dVy = newOffset.y - prevOffset.y;
            const float dVx = newOffset.x - prevOffset.x;
            const bool  flingVertical = std::fabs(dVy) >= std::fabs(dVx);
            const float d = flingVertical ? dVy : dVx;
            // Only fling for a plausible inter-tick gap — a first tick or a
            // long pause (dt huge) is treated as a fresh start, not a fling.
            if(dt > 1e-4 && dt < 0.2 && std::fabs(d) > 0.f){
                owner->startFling(flingVertical, d / static_cast<float>(dt));
            }
        }

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

    Composition::Rect ScrollView::thumbLocalRect(bool vertical){
        const Composition::Rect empty{{0.f, 0.f}, 0.f, 0.f};
        if(child == nullptr){
            return empty;
        }
        const auto & viewRect = getRect();
        const auto & contentRect = child->getRect();
        if(vertical){
            if(!hasVerticalScrollBar || contentRect.h <= viewRect.h){
                return empty;
            }
            const float trackH = viewRect.h - 2.f * kScrollBarMargin;
            const float thumbH = std::max(kMinThumbLength,
                                          trackH * (viewRect.h / contentRect.h));
            const float ratio = std::clamp(
                scrollOffset.y / (contentRect.h - viewRect.h), 0.f, 1.f);
            const float thumbY = ratio * (trackH - thumbH);
            return {{viewRect.w - kScrollBarThickness - kScrollBarMargin,
                     kScrollBarMargin + thumbY},
                    kScrollBarThickness, thumbH};
        }
        if(!hasHorizontalScrollBar || contentRect.w <= viewRect.w){
            return empty;
        }
        const float trackW = viewRect.w - 2.f * kScrollBarMargin;
        const float thumbW = std::max(kMinThumbLength,
                                      trackW * (viewRect.w / contentRect.w));
        const float ratio = std::clamp(
            scrollOffset.x / (contentRect.w - viewRect.w), 0.f, 1.f);
        const float thumbX = ratio * (trackW - thumbW);
        return {{kScrollBarMargin + thumbX,
                 viewRect.h - kScrollBarThickness - kScrollBarMargin},
                thumbW, kScrollBarThickness};
    }

    Composition::Rect ScrollView::trackLocalRect(bool vertical){
        // The strip the thumb slides within: same cross-axis position and
        // thickness as the thumb, spanning the whole track along the main
        // axis. Zero-size when that axis has no bar.
        const Composition::Rect thumb = thumbLocalRect(vertical);
        if(thumb.w <= 0.f || thumb.h <= 0.f){
            return {{0.f, 0.f}, 0.f, 0.f};
        }
        const auto & viewRect = getRect();
        if(vertical){
            return {{thumb.pos.x, kScrollBarMargin},
                    kScrollBarThickness, viewRect.h - 2.f * kScrollBarMargin};
        }
        return {{kScrollBarMargin, thumb.pos.y},
                viewRect.w - 2.f * kScrollBarMargin, kScrollBarThickness};
    }

    int ScrollView::hitTestThumb(const Composition::Point2D & windowPoint){
        const auto origin = offsetFromRoot();
        const Composition::Point2D local{windowPoint.x - origin.x,
                                         windowPoint.y - origin.y};
        auto inRect = [](const Composition::Rect & r,
                         const Composition::Point2D & p){
            return r.w > 0.f && r.h > 0.f
                && p.x >= r.pos.x && p.x <= r.pos.x + r.w
                && p.y >= r.pos.y && p.y <= r.pos.y + r.h;
        };
        if(inRect(thumbLocalRect(true), local)){
            return 1;
        }
        if(inRect(thumbLocalRect(false), local)){
            return 2;
        }
        return 0;
    }

    void ScrollView::dragThumbTo(bool vertical, float pointerAxisWindow, float grab){
        if(child == nullptr){
            return;
        }
        const auto origin = offsetFromRoot();
        const auto & viewRect = getRect();
        const auto & contentRect = child->getRect();
        Composition::Point2D newOffset = scrollOffset;
        if(vertical){
            if(contentRect.h <= viewRect.h){
                return;
            }
            const float trackH = viewRect.h - 2.f * kScrollBarMargin;
            const float thumbH = std::max(kMinThumbLength,
                                          trackH * (viewRect.h / contentRect.h));
            const float span = trackH - thumbH;
            // Desired thumb top in local track coords.
            float thumbY = (pointerAxisWindow - grab) - (origin.y + kScrollBarMargin);
            thumbY = std::clamp(thumbY, 0.f, std::max(0.f, span));
            const float ratio = span > 0.f ? thumbY / span : 0.f;
            newOffset.y = ratio * (contentRect.h - viewRect.h);
        }
        else {
            if(contentRect.w <= viewRect.w){
                return;
            }
            const float trackW = viewRect.w - 2.f * kScrollBarMargin;
            const float thumbW = std::max(kMinThumbLength,
                                          trackW * (viewRect.w / contentRect.w));
            const float span = trackW - thumbW;
            float thumbX = (pointerAxisWindow - grab) - (origin.x + kScrollBarMargin);
            thumbX = std::clamp(thumbX, 0.f, std::max(0.f, span));
            const float ratio = span > 0.f ? thumbX / span : 0.f;
            newOffset.x = ratio * (contentRect.w - viewRect.w);
        }
        setScrollOffset(newOffset); // clamps + schedules the repaint
    }

    AnimationScheduler * ScrollView::scheduler(){
        if(impl_->treeHost_ == nullptr){
            return nullptr;
        }
        FrameBuilder * fb = impl_->treeHost_->frameBuilder();
        return fb != nullptr ? fb->animationScheduler() : nullptr;
    }

    void ScrollView::cancelFling(){
        if(flingAnim_.valid()){
            flingAnim_.cancel();
            flingAnim_ = Composition::AnimationHandle{};
        }
    }

    void ScrollView::startFling(bool vertical, float velocity){
        if(child == nullptr || std::fabs(velocity) < kMinFlingSpeed){
            return;
        }
        AnimationScheduler * sched = scheduler();
        if(sched == nullptr){
            return;
        }
        const auto & viewRect = getRect();
        const auto & contentRect = child->getRect();
        const float maxV = vertical ? std::max(0.f, contentRect.h - viewRect.h)
                                    : std::max(0.f, contentRect.w - viewRect.w);
        const float cur = vertical ? scrollOffset.y : scrollOffset.x;
        float landing = std::clamp(cur + velocity * kFlingProjectSec, 0.f, maxV);
        if(std::fabs(landing - cur) < 1.f){
            return; // already at the end / negligible fling
        }
        Composition::TimingOptions timing;
        timing.durationMs = static_cast<std::uint32_t>(
            std::clamp(std::fabs(landing - cur) * kFlingMsPerPx,
                       kFlingMinMs, kFlingMaxMs));
        cancelFling();
        // The tween fires apply() each scheduler tick; `this` is cancelled
        // in the destructor and on any new user input, so it never outlives
        // the view.
        flingAnim_ = sched->tween<float>(cur, landing,
            [this, vertical](const float & v){
                Composition::Point2D o = scrollOffset;
                if(vertical){ o.y = v; } else { o.x = v; }
                setScrollOffset(o);
            }, timing, Composition::AnimationCurve::EaseOut());
        // Bootstrap the first frame so the scheduler ticks; the FrameBuilder
        // D7.2 auto-pump then keeps requesting frames while the tween is
        // active (mirrors how Button::invalidate kicks off a hover tween).
        scheduleRepaint();
    }

    void ScrollView::handleDragPointer(Native::NativeEventPtr event){
        if(event->type == Native::NativeEvent::CursorMove){
            if(!draggingThumb_){
                return;
            }
            auto *cp = static_cast<Native::CursorMoveParams *>(event->params);
            if(cp == nullptr){
                return;
            }
            dragThumbTo(dragVertical_,
                        dragVertical_ ? cp->position.y : cp->position.x,
                        dragGrab_);
            // E4: sample the offset velocity (px/sec) with light EMA
            // smoothing, for the fling on release.
            const double now = nowSeconds();
            const float curAxis = dragVertical_ ? scrollOffset.y : scrollOffset.x;
            const double dt = now - dragLastTimeSec_;
            if(dt > 1e-4){
                const float instV = (curAxis - dragLastOffset_)
                                  / static_cast<float>(dt);
                dragVelocity_ = 0.6f * instV + 0.4f * dragVelocity_;
            }
            dragLastOffset_  = curAxis;
            dragLastTimeSec_ = now;
            event->handled = true;
            return;
        }
        if(event->type == Native::NativeEvent::LMouseUp){
            if(draggingThumb_){
                draggingThumb_ = false;
                releaseMouse();
                // E4: fling with the release velocity (no-op below threshold).
                startFling(dragVertical_, dragVelocity_);
                event->handled = true;
            }
            return;
        }
        // LMouseDown.
        auto *mp = static_cast<Native::MouseEventParams *>(event->params);
        if(mp == nullptr){
            return;
        }
        bool vertical = false;
        float grab = 0.f;
        const int thumb = hitTestThumb(mp->position);
        if(thumb != 0){
            // Press on the thumb — grab it at the cursor so it does not jump.
            vertical = (thumb == 1);
            const auto origin = offsetFromRoot();
            const auto tr = thumbLocalRect(vertical);
            grab = vertical ? (mp->position.y - (origin.y + tr.pos.y))
                            : (mp->position.x - (origin.x + tr.pos.x));
        }
        else {
            // Press on the track off the thumb (decision #2): jump the thumb
            // center under the pointer, then drag from there. Ignore presses
            // that are neither thumb nor track — let them bubble.
            const auto origin = offsetFromRoot();
            const Composition::Point2D local{mp->position.x - origin.x,
                                             mp->position.y - origin.y};
            auto inRect = [](const Composition::Rect & r,
                             const Composition::Point2D & p){
                return r.w > 0.f && r.h > 0.f
                    && p.x >= r.pos.x && p.x <= r.pos.x + r.w
                    && p.y >= r.pos.y && p.y <= r.pos.y + r.h;
            };
            if(inRect(trackLocalRect(true), local)){
                vertical = true;
            }
            else if(inRect(trackLocalRect(false), local)){
                vertical = false;
            }
            else {
                return;
            }
            const auto tr = thumbLocalRect(vertical);
            grab = vertical ? (tr.h * 0.5f) : (tr.w * 0.5f);
            dragThumbTo(vertical, vertical ? mp->position.y : mp->position.x, grab);
        }
        draggingThumb_ = true;
        dragVertical_  = vertical;
        dragGrab_      = grab;
        // E4: a new drag cancels any in-flight fling and reseeds velocity.
        cancelFling();
        dragLastOffset_  = vertical ? scrollOffset.y : scrollOffset.x;
        dragLastTimeSec_ = nowSeconds();
        dragVelocity_    = 0.f;
        captureMouse();
        event->handled = true;
    }

    void ScrollView::paintAfterChildren(Composition::PaintContext & pc){
        // V3: close the content clip opened in `paint`.
        pc.displayList.append(Composition::DrawOp::makePopClip());

        // V4: emit the scroll-bar thumbs AFTER the PopClip so they draw
        // outside the viewport scissor. `thumbLocalRect` (E1) computes the
        // per-axis thumb in local space; `pc.offset` lifts it to absolute
        // window coords (the 4.7 walker no longer applies a per-view replay
        // translation). Empty rect (no overflow) → no bar.
        const float ox = pc.offset.x;
        const float oy = pc.offset.y;
        auto emitThumb = [&](const Composition::Rect & t){
            if(t.w <= 0.f || t.h <= 0.f){
                return;
            }
            Composition::RoundedRect rr{
                {ox + t.pos.x, oy + t.pos.y}, t.w, t.h,
                kScrollBarRadius, kScrollBarRadius};
            pc.displayList.append(Composition::DrawOp{
                rr, Composition::ColorBrush(kScrollBarColor)});
        };
        emitThumb(thumbLocalRect(true));
        emitThumb(thumbLocalRect(false));
    }

    bool ScrollView::clipsContentSubtree() const{
        return true;
    }

    ScrollView::~ScrollView(){
        // E4: stop any in-flight fling so the scheduler never invokes its
        // `this`-capturing callback after this view is destroyed.
        cancelFling();
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
