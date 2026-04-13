#include "../Compositor.h"
#include "VisualTree.h"
#include "ResourceFactory.h"
#include <algorithm>
#include <cmath>

namespace OmegaWTK::Composition {

namespace {
    constexpr float kMaxTextureDimension = 16384.f;
#if defined(TARGET_MACOS)
    constexpr float kLogicalScaleFloor = 2.f;
#else
    constexpr float kLogicalScaleFloor = 1.f;
#endif
    constexpr float kMaxLogicalLayerDimension = kMaxTextureDimension / kLogicalScaleFloor;

    static inline Composition::Rect sanitizeCommandRect(const Composition::Rect & candidate,const Composition::Rect & fallback){
        Composition::Rect saneFallback = fallback;
        if(!std::isfinite(saneFallback.pos.x)){
            saneFallback.pos.x = 0.f;
        }
        if(!std::isfinite(saneFallback.pos.y)){
            saneFallback.pos.y = 0.f;
        }
        if(!std::isfinite(saneFallback.w) || saneFallback.w <= 0.f){
            saneFallback.w = 1.f;
        }
        if(!std::isfinite(saneFallback.h) || saneFallback.h <= 0.f){
            saneFallback.h = 1.f;
        }
        saneFallback.w = std::clamp(saneFallback.w,1.f,kMaxLogicalLayerDimension);
        saneFallback.h = std::clamp(saneFallback.h,1.f,kMaxLogicalLayerDimension);

        Composition::Rect sane = candidate;
        if(!std::isfinite(sane.pos.x)){
            sane.pos.x = saneFallback.pos.x;
        }
        if(!std::isfinite(sane.pos.y)){
            sane.pos.y = saneFallback.pos.y;
        }
        if(!std::isfinite(sane.w) || sane.w <= 0.f){
            sane.w = saneFallback.w;
        }
        if(!std::isfinite(sane.h) || sane.h <= 0.f){
            sane.h = saneFallback.h;
        }
        sane.w = std::clamp(sane.w,1.f,kMaxLogicalLayerDimension);
        sane.h = std::clamp(sane.h,1.f,kMaxLogicalLayerDimension);
        return sane;
    }
}


void Compositor::executeCurrentCommand(){

    if(currentCommand->type == CompositorCommand::Layer){
        auto params = (CompositorLayerCommand *)currentCommand.get();
        if(params->layer == nullptr){
            currentCommand->status.set(CommandStatus::Failed);
            return;
        }
        /// Resize Command
        if(params->subtype == CompositorLayerCommand::Resize){
            auto priorRect = sanitizeCommandRect(
                    params->layer->getLayerRect(),
                    Composition::Rect{Composition::Point2D{0.f,0.f},1.f,1.f});
            auto layerRect = priorRect;
            layerRect.pos.x += (float)params->delta_x;
            layerRect.pos.y += (float)params->delta_y;
            layerRect.w += (float)params->delta_w;
            layerRect.h += (float)params->delta_h;
            layerRect = sanitizeCommandRect(layerRect,priorRect);
            params->layer->resize(layerRect);
        }
        else {
            // Layer effect commands (shadow, transform) are no longer applied via
            // native layer properties. Effects are now draw-time Canvas operations.
            // Effect commands are accepted but ignored until the draw-time path is implemented.
        }
    }
    else if(currentCommand->type == CompositorCommand::View){
        auto params = (CompositorViewCommand *)currentCommand.get();
        if(params->subType == CompositorViewCommand::Resize){
            auto currentRect = sanitizeCommandRect(
                    params->viewPtr->getRect(),
                    Composition::Rect{Composition::Point2D{0.f,0.f},1.f,1.f});
            auto nextRect = currentRect;
            nextRect.pos.x += static_cast<float>(params->delta_x);
            nextRect.pos.y += static_cast<float>(params->delta_y);
            nextRect.w += static_cast<float>(params->delta_w);
            nextRect.h += static_cast<float>(params->delta_h);
            nextRect = sanitizeCommandRect(nextRect,currentRect);
            params->viewPtr->resize(nextRect);
        }
    }

    currentCommand->status.set(CommandStatus::Ok);
}

void Compositor::onQueueDrained(){
    // Window surfaces are drained from the scheduler loop on
    // deposit-callback wake, not here.
}


};
