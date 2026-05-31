#ifndef OMEGAWTK_UI_LAYOUTBEHAVIORS_H
#define OMEGAWTK_UI_LAYOUTBEHAVIORS_H

#include "omegaWTK/UI/Layout.h"

namespace OmegaWTK {

// Phase 4.5: `LegacyResizeCoordinatorBehavior` deleted alongside
// `ViewResizeCoordinator`. The widget-layout entry point
// (`runWidgetLayout`) now drives the parent's `LayoutManager` directly.

class StackLayoutBehavior : public LayoutBehavior {
public:
    MeasureResult measure(LayoutNode & node,const LayoutContext & ctx) override;
    void arrange(LayoutNode & node,const LayoutContext & ctx) override;
};

}

#endif
