
#include "omegaWTK/UI/Widget.h"
#include "omegaWTK/Core/Core.h"

#ifndef OMEGAWTK_WIDGETS_BASICWIDGETS_H
#define OMEGAWTK_WIDGETS_BASICWIDGETS_H

namespace OmegaWTK {

// /**
//  * @brief A single view widget responsible for managing one view's capability.
//  * 
//  */

// class OMEGAWTK_EXPORT WrapperWidget {
// public:
//     static SharedHandle<WrapperWidget> CreateVideoViewWrapper(const Core::Rect & rect,WidgetPtr parent);
//     static SharedHandle<WrapperWidget> CreateSVGViewWrapper(const Core::Rect & rect,WidgetPtr parent);
//     static SharedHandle<WrapperWidget> CreateUIViewWrapper(const Core::Rect & rect,WidgetPtr parent);
//     static SharedHandle<WrapperWidget> CreateScollViewWrapper(const Core::Rect & rect,WidgetPtr parent);

//     SharedHandle<View> getUnderlyingView();
// };

   /**
 * @brief A widget designed for holding other widgets (No rendering or native event handling)
 * 
 */
class OMEGAWTK_EXPORT ContainerWidget : public Widget {
public:
    static SharedHandle<ContainerWidget> Create(const Core::Rect & rect,WidgetPtr parent);

};

/**
 * @brief Similar to ContainerWidget except all widgets can be moved (drag-dropped, animated) with native events or object methods.
 * 
 */

class OMEGAWTK_EXPORT ModularContainerWidget : public Widget {
    static SharedHandle<ContainerWidget> Create(const Core::Rect & rect,WidgetPtr parent);
};


class OMEGAWTK_EXPORT Scrollable : public Widget {

};


}
#endif //OMEGAWTK_WIDGETS_BASICWIDGETS_H