#include "omegaWTK/Core/Core.h"
#include "Geometry.h"
#include "Canvas.h"

#include <cstdint>

#ifndef OMEGAWTK_COMPOSITION_COMPOSITEFRAME_H
#define OMEGAWTK_COMPOSITION_COMPOSITEFRAME_H

namespace OmegaWTK::Composition {

class Layer;

struct CompositeFrame {
    struct WidgetSlice {
        Composition::Rect bounds;
        Composition::Point2D windowOffset {0.f, 0.f};
        OmegaCommon::Vector<VisualCommand> commands;
        OmegaCommon::Vector<CanvasEffect> effects;
        struct {
            float r = 0.f, g = 0.f, b = 0.f, a = 0.f;
        } background;
        Layer *targetLayer = nullptr;
    };
    OmegaCommon::Vector<WidgetSlice> slices;
    uint64_t sizeGeneration = 0;
};

}

#endif
