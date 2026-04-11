#ifndef OMEGAWTK_COMPOSITION_PATHIMPL_H
#define OMEGAWTK_COMPOSITION_PATHIMPL_H

#include "omegaWTK/Composition/Path.h"
#include <OmegaGTE.h>

namespace OmegaWTK::Composition {

struct Path::Impl {
    struct Segment {
        OmegaGTE::GVectorPath2D path;
        OmegaGTE::GVectorPath2D final_path_a;
        OmegaGTE::GVectorPath2D final_path_b;
        bool closed = false;
        explicit Segment(OmegaGTE::GPoint2D startPoint)
            : path(startPoint), final_path_a(startPoint), final_path_b(startPoint) {}
    };
    OmegaCommon::Vector<Segment> segments;
    unsigned currentStroke;
    float arcPrecision;
    Core::SharedPtr<Brush> pathBrush;
};

}

#endif
