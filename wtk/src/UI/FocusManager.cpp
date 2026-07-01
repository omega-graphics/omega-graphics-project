#include "omegaWTK/UI/FocusManager.h"
#include "ViewImpl.h"   // View::Impl — focused_ / lastReason_ access (friend)

#include "omegaWTK/Native/NativeEvent.h"

namespace OmegaWTK {

using Native::NativeEvent;
using Native::NativeEventPtr;

FocusManager::FocusManager() = default;

FocusManager::~FocusManager() = default;

void FocusManager::setFocus(View * view, FocusReason reason){
    // Re-focusing the already-focused view: refresh the reason only.
    // No flag flip and no event churn — a click that lands on the
    // already-focused control should not re-fire FocusGained/FocusLost.
    if(view == currentlyFocused_){
        lastReason_ = reason;
        if(view != nullptr){
            view->impl_->lastReason_ = reason;
        }
        return;
    }

    View * previous = currentlyFocused_;

    // Commit the settled focus state *before* emitting, so any handler
    // that reads View::isFocused() / lastFocusReason() observes the new
    // truth rather than a half-applied transition.
    if(previous != nullptr){
        previous->impl_->focused_ = false;
    }
    currentlyFocused_ = view;
    lastReason_ = reason;
    if(view != nullptr){
        view->impl_->focused_ = true;
        view->impl_->lastReason_ = reason;
    }

    // §2.3a F2 emit order: FocusGained on the new view (params: the
    // previous View*), then FocusLost on the previous view (params: the
    // new View*). The View* is passed as the event's non-owned void*
    // params — NativeEvent's destructor does not free FocusGained /
    // FocusLost params, so there is nothing to allocate or leak.
    if(view != nullptr){
        view->emit(NativeEventPtr(new NativeEvent(NativeEvent::FocusGained, previous)));
    }
    if(previous != nullptr){
        previous->emit(NativeEventPtr(new NativeEvent(NativeEvent::FocusLost, view)));
    }
}

View * FocusManager::focusedView() const{
    return currentlyFocused_;
}

FocusReason FocusManager::lastFocusReason() const{
    return lastReason_;
}

void FocusManager::clearFocus(){
    if(currentlyFocused_ == nullptr){
        return;
    }
    View * previous = currentlyFocused_;
    previous->impl_->focused_ = false;
    currentlyFocused_ = nullptr;
    // lastReason_ retained: it still reflects why `previous` was focused,
    // which a focus-ring consumer may want to inspect after the clear.
    previous->emit(NativeEventPtr(new NativeEvent(NativeEvent::FocusLost, nullptr)));
}

void FocusManager::setRoot(View * root){
    root_ = root;
}

namespace {

// §2.3a F4: pre-order collection of the tab-focusable views under `view`.
// Order is parent-before-children (the view is appended before its
// subtree is walked), giving the DOM tab-order the traversal contract
// promises. `NoFocus` / `ClickFocus`-only views are skipped here rather
// than filtered afterward so indices in `out` line up with real Tab
// stops. Non-owning throughout — the widget tree owns every View.
void collectTabFocusable(View * view, OmegaCommon::Vector<View *> & out){
    if(view == nullptr){
        return;
    }
    if(view->isTabFocusable()){
        out.push_back(view);
    }
    for(View * child : view->subviews()){
        collectTabFocusable(child, out);
    }
}

// §2.3a F5: is `target` still present in the subtree rooted at `root`?
// Pure pointer comparison — `target` is NEVER dereferenced, so this is
// safe to call with a pointer that may already have been freed: a dead
// pointer simply fails to match any live node and returns false. This is
// how `popAndRestore` tolerates a restoration target that was detached or
// destroyed while a popup was up.
//
// Caveat (ABA): if a View is freed and a *different* View is later
// allocated at the same address and attached under `root`, this returns
// a false positive and focus is restored to the wrong view. WTK has no
// weak-view tracking (Chromium uses a ViewTracker for exactly this), so
// the tree walk is the pragmatic guard; the window in which a same-address
// reuse both happens and re-attaches under the same root before the pop is
// vanishingly small in practice.
bool viewInSubtree(View * root, View * target){
    if(root == nullptr){
        return false;
    }
    if(root == target){
        return true;
    }
    for(View * child : root->subviews()){
        if(viewInSubtree(child, target)){
            return true;
        }
    }
    return false;
}

}

bool FocusManager::focusNext(){
    OmegaCommon::Vector<View *> order;
    collectTabFocusable(root_, order);
    if(order.empty()){
        return false;
    }
    // Locate the current holder in the traversal set. A focused view that
    // is not in the set (e.g. an overlay outside `root_`) leaves index -1,
    // and traversal enters at the first stop — the intuitive "start from
    // the top" behavior for Tab.
    std::size_t idx = 0;
    bool found = false;
    for(std::size_t i = 0; i < order.size(); ++i){
        if(order[i] == currentlyFocused_){
            idx = i;
            found = true;
            break;
        }
    }
    View * next = found ? order[(idx + 1) % order.size()] : order.front();
    setFocus(next, FocusReason::Tab);
    return true;
}

bool FocusManager::focusPrevious(){
    OmegaCommon::Vector<View *> order;
    collectTabFocusable(root_, order);
    if(order.empty()){
        return false;
    }
    std::size_t idx = 0;
    bool found = false;
    for(std::size_t i = 0; i < order.size(); ++i){
        if(order[i] == currentlyFocused_){
            idx = i;
            found = true;
            break;
        }
    }
    // Not-found lands on the LAST stop: Shift-Tab from a fresh window
    // should enter at the end, matching Qt / browser backtab. Wrap uses
    // `+ size - 1` to avoid an unsigned underflow at idx 0.
    View * prev = found
        ? order[(idx + order.size() - 1) % order.size()]
        : order.back();
    setFocus(prev, FocusReason::Backtab);
    return true;
}

void FocusManager::pushRestorationPoint(){
    // Capture the raw current holder (may be null). No validation here —
    // it is the caller's snapshot of "who had focus", checked for
    // liveness only at pop time.
    restorationStack_.push_back(currentlyFocused_);
}

void FocusManager::popAndRestore(){
    if(restorationStack_.empty()){
        return;
    }
    View * saved = restorationStack_.back();
    restorationStack_.pop_back();
    // Tolerance (see viewInSubtree): a non-null capture that is no longer
    // in the live tree was detached/destroyed while the popup was up.
    // Re-focusing it is both meaningless and — via setFocus's handling of
    // the new holder's flags/emit — a dereference of freed memory, so skip
    // restoration entirely and leave focus as the popup left it.
    if(saved != nullptr && !viewInSubtree(root_, saved)){
        return;
    }
    // saved == null falls through here: setFocus(nullptr, …) clears focus,
    // restoring the "nothing was focused" state the point captured. A
    // valid non-null saved is re-focused with FocusReason::Popup so the
    // focus ring returns if it had been tab-focused before.
    setFocus(saved, FocusReason::Popup);
}

}
