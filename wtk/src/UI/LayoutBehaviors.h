#ifndef OMEGAWTK_UI_LAYOUTBEHAVIORS_H
#define OMEGAWTK_UI_LAYOUTBEHAVIORS_H

#include "omegaWTK/UI/Layout.h"

namespace OmegaWTK {

class LegacyResizeCoordinatorBehavior : public LayoutBehavior {
    ViewResizeCoordinator & coordinator_;
public:
    explicit LegacyResizeCoordinatorBehavior(ViewResizeCoordinator & coordinator);
    MeasureResult measure(LayoutNode & node,const LayoutContext & ctx) override;
    void arrange(LayoutNode & node,const LayoutContext & ctx) override;
};

class StackLayoutBehavior : public LayoutBehavior {
public:
    MeasureResult measure(LayoutNode & node,const LayoutContext & ctx) override;
    void arrange(LayoutNode & node,const LayoutContext & ctx) override;
};

}

#endif
