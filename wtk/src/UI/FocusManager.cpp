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

}
