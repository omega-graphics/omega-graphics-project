#include "BasicWidgets.h"

#ifndef OMEGAWTK_WIDGETS_USERINPUTS_H
#define OMEGAWTK_WIDGETS_USERINPUTS_H

namespace OmegaWTK {
    class OMEGAWTK_EXPORT TextInput : public Container {
    public:
        WIDGET_CONSTRUCTOR()
    };

    // QUESTION: Should this inherit from Container or Widget? 
    // We can already customize the look through UIView, but it shouldn't we try to keep the API simple?
    class OMEGAWTK_EXPORT Button : public Container {
    public:
        WIDGET_CONSTRUCTOR()
    };

    class OMEGAWTK_EXPORT Dropdown : public Container {
    public:
        WIDGET_CONSTRUCTOR()
    };

    class OMEGAWTK_EXPORT Slider : public Container {
    public:
        WIDGET_CONSTRUCTOR()
    };

    
}

#endif //OMEGAWTK_WIDGETS_USERINPUTS_H