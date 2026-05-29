#include "omegaWTK/Core/Core.h"
#include "Geometry.h"
#include "DisplayList.h"

#include <cstdint>

#ifndef OMEGAWTK_COMPOSITION_COMPOSITEFRAME_H
#define OMEGAWTK_COMPOSITION_COMPOSITEFRAME_H

namespace OmegaWTK::Composition {

class Layer;

struct CompositeFrame {
    struct WidgetSlice {
        Composition::Rect bounds;
        Composition::Point2D windowOffset {0.f, 0.f};
        // Tier 4 §4.1/4.2: the DrawOp recording carried straight from
        // FrameBuilder::submitDisplayList. The backend flush dispatches
        // these via renderToTarget(DrawOp::Type). (The old
        // Vector<VisualCommand> commands field was deleted in 4.2.)
        Composition::DisplayList ops;
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
