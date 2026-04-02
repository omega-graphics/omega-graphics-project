#include "BasicWidgets.h"

#ifndef OMEGAWTK_WIDGETS_USERINPUTS_H
#define OMEGAWTK_WIDGETS_USERINPUTS_H

namespace OmegaWTK {
    // @note We Should use OmegaCommon::UString string for all text widgets. (Unicode support for rendering all languages.)
    class OMEGAWTK_EXPORT Label : public Widget {
    public:
        WIDGET_CONSTRUCTOR()
    };

    class OMEGAWTK_EXPORT TextInput : public Widget {
    public:
        WIDGET_CONSTRUCTOR()
    };

    // QUESTION: Should this inherit from Container or Widget? 
    // We can already customize the look through UIView, but it shouldn't we try to keep the API simple?
    class OMEGAWTK_EXPORT Button : public Widget {
    public:
        WIDGET_CONSTRUCTOR()
    };

    class OMEGAWTK_EXPORT Dropdown : public Widget {
    public:
        WIDGET_CONSTRUCTOR()
    };

    class OMEGAWTK_EXPORT Slider : public Widget {
    public:
        WIDGET_CONSTRUCTOR()
    };

    
}

#endif //OMEGAWTK_WIDGETS_USERINPUTS_H