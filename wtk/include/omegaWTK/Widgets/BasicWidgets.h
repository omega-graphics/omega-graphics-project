#include "omegaWTK/UI/Widget.h"
#include "omegaWTK/Core/Core.h"

#ifndef OMEGAWTK_WIDGETS_BASICWIDGETS_H
#define OMEGAWTK_WIDGETS_BASICWIDGETS_H

namespace OmegaWTK {

 template<class ViewT>
class OMEGAWTK_EXPORT ContainerWidget : public Widget {
public:
    static SharedHandle<WrapperWidget<ViewT>> Create(const Core::Rect & rect,WidgetPtr parent);
    SharedHandle<ViewT> getUnderlyingView();
};

    /**
 * @brief A single view widget responsible for managing one view's capability.
 * 
 */
 template<class ViewT>
class OMEGAWTK_EXPORT WrapperWidget {
public:
    static SharedHandle<WrapperWidget<ViewT>> Create(const Core::Rect & rect,WidgetPtr parent);
    SharedHandle<ViewT> getUnderlyingView();
};
}

#endif //OMEGAWTK_WIDGETS_BASICWIDGETS_H