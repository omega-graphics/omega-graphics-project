
#include "omegaWTK/UI/Widget.h"
#include "omegaWTK/Core/Core.h"

#ifndef OMEGAWTK_WIDGETS_BASICWIDGETS_H
#define OMEGAWTK_WIDGETS_BASICWIDGETS_H

/**
* Every Widget Constructor comes with two default parameters: The rect, and the parent widget.
*/
#define WIDGET_CONSTRUCTOR(args...) static SharedHandle<Widget> Create(const Core::Rect & rect,WidgetPtr parent,...args);
#define WIDGET_CONSTRUCTOR_IMPL(args...) Create(const Core::Rect & rect,WidgetPtr parent,...args)
#define WIDGET_CREATE(type,rect,parent,args...) make<type>(rect,parent,...args)

namespace OmegaWTK {

    

// /**
//  * @brief A single view widget responsible for managing one view's capability.
//  * 
//  */

// class OMEGAWTK_EXPORT WrapperWidget : public Widget {
// public:
//     static SharedHandle<WrapperWidget> CreateVideoViewWrapper(const Core::Rect & rect,WidgetPtr parent);
//     static SharedHandle<WrapperWidget> CreateSVGViewWrapper(const Core::Rect & rect,WidgetPtr parent);
//     static SharedHandle<WrapperWidget> CreateUIViewWrapper(const Core::Rect & rect,WidgetPtr parent);
//     static SharedHandle<WrapperWidget> CreateScollViewWrapper(const Core::Rect & rect,WidgetPtr parent);

//     SharedHandle<View> getUnderlyingView();
// };

   /**
 * @brief A widget designed for holding other widgets (No rendering or native event handling can change Widget positioning)
 * 
 */
class OMEGAWTK_EXPORT Container: public Widget {
public:
    WIDGET_CONSTRUCTOR()

};

/**
 * @brief Similar to `Container` except all widgets can be moved (drag-dropped, animated) with native events or object methods.
 * 
 */

class OMEGAWTK_EXPORT AnimatedContainer : public Widget {
public:
    WIDGET_CONSTRUCTOR()
};


class OMEGAWTK_EXPORT ScrollableContainer : public Widget {
    WIDGET_CONSTRUCTOR()
};


}
#endif //OMEGAWTK_WIDGETS_BASICWIDGETS_H