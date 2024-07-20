#include "omegaWTK/Widgets/BasicWidgets.h"

namespace OmegaWTK {
    WidgetPtr Container::WIDGET_CONSTRUCTOR_IMPL(){
        return WIDGET_CREATE(Widget,rect,parent);
    }
}