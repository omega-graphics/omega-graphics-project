#include "ViewImpl.h"

namespace OmegaWTK {

void ViewResizeCoordinator::attachView(View * parent){
    parentView = parent;
}

void ViewResizeCoordinator::registerChild(View * child,const ChildResizeSpec & spec){
    if(child == nullptr){
        return;
    }
    auto & state = childState[child];
    state.spec = spec;
    state.baselineChildRect = child->getRect();
    if(parentView != nullptr){
        state.baselineParentRect = parentView->getRect();
    }
    state.hasBaseline = true;
}

void ViewResizeCoordinator::updateChildSpec(View * child,const ChildResizeSpec & spec){
    if(child == nullptr){
        return;
    }
    auto entry = childState.find(child);
    if(entry == childState.end()){
        registerChild(child,spec);
        return;
    }
    entry->second.spec = spec;
}

void ViewResizeCoordinator::unregisterChild(View * child){
    if(child == nullptr){
        return;
    }
    childState.erase(child);
}

void ViewResizeCoordinator::beginResizeSession(std::uint64_t sessionId){
    activeSessionId = sessionId;
    (void)activeSessionId;
    Core::Rect parentBaseline {Core::Position{0.f,0.f},1.f,1.f};
    if(parentView != nullptr){
        parentBaseline = parentView->getRect();
    }
    for(auto & entry : childState){
        auto * child = entry.first;
        if(child == nullptr){
            continue;
        }
        auto & state = entry.second;
        state.baselineParentRect = parentBaseline;
        state.baselineChildRect = child->getRect();
        state.hasBaseline = true;
    }
}

Core::Rect ViewResizeCoordinator::clampRectToParent(const Core::Rect & requested,
                                                    const Core::Rect & parentContentRect,
                                                    const ChildResizeSpec & spec){
    auto fallback = ViewInternal::sanitizeRect(parentContentRect,Core::Rect{Core::Position{0.f,0.f},1.f,1.f});
    auto parent = ViewInternal::sanitizeRect(parentContentRect,fallback);
    auto output = ViewInternal::sanitizeRect(requested,parent);

    const float minWidth = std::max(1.f,std::isfinite(spec.clamp.minWidth) ? spec.clamp.minWidth : 1.f);
    const float minHeight = std::max(1.f,std::isfinite(spec.clamp.minHeight) ? spec.clamp.minHeight : 1.f);
    const float maxWidth = std::isfinite(spec.clamp.maxWidth) ? spec.clamp.maxWidth : ViewInternal::kMaxViewDimension;
    const float maxHeight = std::isfinite(spec.clamp.maxHeight) ? spec.clamp.maxHeight : ViewInternal::kMaxViewDimension;

    if(spec.resizable){
        output.w = ViewInternal::clampAxis(output.w,minWidth,std::max(minWidth,maxWidth));
        output.h = ViewInternal::clampAxis(output.h,minHeight,std::max(minHeight,maxHeight));
    }
    else {
        output.w = std::max(1.f,output.w);
        output.h = std::max(1.f,output.h);
    }

    output.w = std::min(output.w,std::max(1.f,parent.w));
    output.h = std::min(output.h,std::max(1.f,parent.h));

    const float minX = parent.pos.x;
    const float minY = parent.pos.y;
    const float maxX = parent.pos.x + std::max(0.f,parent.w - output.w);
    const float maxY = parent.pos.y + std::max(0.f,parent.h - output.h);
    output.pos.x = ViewInternal::clampAxis(output.pos.x,minX,maxX);
    output.pos.y = ViewInternal::clampAxis(output.pos.y,minY,maxY);
    return output;
}

Core::Rect ViewResizeCoordinator::resolveChildRect(View * child,
                                                   const Core::Rect & requested,
                                                   const Core::Rect & parentContentRect){
    if(child == nullptr){
        return requested;
    }
    auto stateIt = childState.find(child);
    if(stateIt == childState.end()){
        registerChild(child,{});
        stateIt = childState.find(child);
        if(stateIt == childState.end()){
            return requested;
        }
    }

    auto & state = stateIt->second;
    auto output = ViewInternal::sanitizeRect(requested,child->getRect());
    auto parent = ViewInternal::sanitizeRect(parentContentRect,Core::Rect{Core::Position{0.f,0.f},1.f,1.f});

    switch(state.spec.policy){
        case ChildResizePolicy::Fill: {
            if(state.spec.resizable){
                const float gx = std::max(0.f,state.spec.growWeightX);
                const float gy = std::max(0.f,state.spec.growWeightY);
                output.w = parent.w * (gx > 0.f ? std::min(1.f,gx) : 1.f);
                output.h = parent.h * (gy > 0.f ? std::min(1.f,gy) : 1.f);
                output.pos.x = parent.pos.x;
                output.pos.y = parent.pos.y;
            }
            break;
        }
        case ChildResizePolicy::Proportional: {
            if(state.spec.resizable && state.hasBaseline &&
               state.baselineParentRect.w > 0.f &&
               state.baselineParentRect.h > 0.f){
                const float scaleX = parent.w / state.baselineParentRect.w;
                const float scaleY = parent.h / state.baselineParentRect.h;
                output.w = state.baselineChildRect.w * scaleX;
                output.h = state.baselineChildRect.h * scaleY;
                output.pos.x = parent.pos.x + (state.baselineChildRect.pos.x - state.baselineParentRect.pos.x) * scaleX;
                output.pos.y = parent.pos.y + (state.baselineChildRect.pos.y - state.baselineParentRect.pos.y) * scaleY;
            }
            break;
        }
        case ChildResizePolicy::Fixed:
        case ChildResizePolicy::FitContent:
        default:
            break;
    }

    output = clampRectToParent(output,parent,state.spec);
    return output;
}

void ViewResizeCoordinator::resolve(const Core::Rect & parentContentRect){
    for(auto & entry : childState){
        auto * child = entry.first;
        if(child == nullptr){
            continue;
        }
        auto resolved = resolveChildRect(child,child->getRect(),parentContentRect);
        if(!ViewInternal::sameRect(resolved,child->getRect())){
            child->resize(resolved);
        }
    }
}

}
