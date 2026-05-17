#include "FrameBuilder.h"

#include "AppWindowImpl.h"
#include "WidgetTreeHost.h"

#include "omegaWTK/Composition/Canvas.h"
#include "omegaWTK/Composition/CompositeFrame.h"
#include "omegaWTK/Composition/CompositorClient.h"
#include "omegaWTK/Composition/CompositorSurface.h"
#include "omegaWTK/Composition/DisplayList.h"
#include "omegaWTK/UI/AppWindow.h"
#include "omegaWTK/UI/View.h"

namespace OmegaWTK {

namespace {
// Phase 3.2: AppWindow::activeFrameBuilder() reads this; FrameBuilder
// sets it during the outermost beginFrame/endFrame so UIView::update
// (and SVGView in Phase 3.3) can route their DisplayList submissions
// to the right FrameBuilder without holding a back-pointer to
// AppWindow. Paint runs on the UI thread; a single static is
// sufficient. If paint ever multi-threads, this becomes thread_local.
FrameBuilder * g_activeFrameBuilder = nullptr;
}

FrameBuilder::FrameBuilder(AppWindow & window): window_(window) {}

FrameBuilder::~FrameBuilder() = default;

void FrameBuilder::beginFrame(){
    if(depth_++ > 0){
        return;
    }

    auto * impl = window_.impl_.get();
    if(impl == nullptr){
        baselineVisualCount_ = 0;
        return;
    }
    auto * treeHost = impl->widgetTreeHost.get();
    if(treeHost != nullptr){
        if(impl->proxy.getFrontendPtr() == nullptr){
            impl->proxy.setFrontendPtr(treeHost->compPtr());
        }
        auto desiredLane = treeHost->laneId();
        if(desiredLane != 0 && impl->proxy.getSyncLaneId() != desiredLane){
            impl->proxy.setSyncLaneId(desiredLane);
        }
    }

    // Capture the windowScopedPaint flag once per frame so a flip
    // mid-paint cannot leave a CompositeFrame attached to the proxy
    // with no owner reading it.
    windowScopedPaintActive_ = impl->windowScopedPaint_;

    pending_.clear();

    if(windowScopedPaintActive_){
        // Phase 3.2: allocate a window-level CompositeFrame and
        // attach it to the AppWindow's proxy. windowCanvas's
        // pushFrame (during endFrame's replay loop) deposits slices
        // here; endFrame hands the frame off to the window surface.
        compositeFrame_ = std::make_shared<Composition::CompositeFrame>();
        impl->proxy.setActiveCompositeFrame(compositeFrame_.get());
    }

    baselineVisualCount_ = 0;
    if(impl->windowCanvas_ != nullptr){
        auto frame = impl->windowCanvas_->getCurrentFrame();
        if(frame != nullptr){
            baselineVisualCount_ = frame->currentVisuals.size();
        }
    }

    g_activeFrameBuilder = this;
}

void FrameBuilder::endFrame(){
    if(depth_ == 0){
        // endFrame without a matching beginFrame — defensive no-op.
        return;
    }
    if(--depth_ > 0){
        return;
    }

    auto * impl = window_.impl_.get();
    if(impl == nullptr){
        g_activeFrameBuilder = nullptr;
        return;
    }

    if(windowScopedPaintActive_ && impl->windowCanvas_ != nullptr){
        // Phase 3.2: drain pending submissions in tree order. Each
        // submission stamps its captured window-offset onto the
        // window canvas's current frame (the window canvas has no
        // ownerView_, so its nextFrame() does not overwrite the
        // windowOffset field) and replays the DisplayList. sendFrame
        // pushes a slice onto compositeFrame_ via the AppWindow
        // proxy attached at beginFrame.
        for(auto & sub : pending_){
            auto current = impl->windowCanvas_->getCurrentFrame();
            if(current != nullptr){
                current->windowOffset = sub.windowOffset;
            }
            Composition::DisplayListReplay::replay(sub.list, *impl->windowCanvas_);
            impl->windowCanvas_->sendFrame();
        }
    }
    else {
        // Flag off: fall back to the Phase 3.1 baseline-grew check.
        // No submissions queued; only the direct-draw path matters,
        // and the existing scenes do not exercise it.
        if(impl->windowCanvas_ != nullptr){
            auto frame = impl->windowCanvas_->getCurrentFrame();
            if(frame != nullptr &&
               frame->currentVisuals.size() > baselineVisualCount_){
                impl->windowCanvas_->sendFrame();
            }
        }
    }

    pending_.clear();

    if(windowScopedPaintActive_){
        // Detach the CompositeFrame from the proxy before depositing
        // so any post-endFrame draws (there shouldn't be any) do
        // not silently re-enter this frame.
        impl->proxy.setActiveCompositeFrame(nullptr);
        if(compositeFrame_ != nullptr &&
           !compositeFrame_->slices.empty() &&
           impl->windowSurface != nullptr){
            impl->windowSurface->deposit(compositeFrame_);
        }
        compositeFrame_.reset();
        windowScopedPaintActive_ = false;
    }

    g_activeFrameBuilder = nullptr;
}

void FrameBuilder::submitView(View * view, Composition::DisplayList list){
    if(view == nullptr){
        return;
    }
    PendingSubmission sub;
    sub.windowOffset = view->computeWindowOffset();
    sub.list = std::move(list);
    pending_.push_back(std::move(sub));
}

// Phase 3.2: static accessor lives on AppWindow but reads the
// FrameBuilder-internal slot, so the AppWindow header does not have
// to expose the static storage. Defined here for the same reason
// the slot lives here.
FrameBuilder * AppWindow::activeFrameBuilder(){
    return g_activeFrameBuilder;
}

} // namespace OmegaWTK
