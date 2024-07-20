#include "BasicWidgets.h"

#ifndef OMEGAWTK_WIDGETS_USERINPUTS_H
#define OMEGAWTK_WIDGETS_USERINPUTS_H

namespace OmegaWTK {

    class OMEGAWTK_EXPORT Label : public Widget {
    public:
        WIDGET_CONSTRUCTOR(OmegaCommon::UString str)
    };

    class OMEGAWTK_EXPORT TextInput : public Widget {
    public:
        WIDGET_CONSTRUCTOR()
    };


    class OMEGAWTK_EXPORT Button : public Container {
    public:
        WIDGET_CONSTRUCTOR()
    };

    
}

#endif //OMEGAWTK_WIDGETS_USERINPUTS_H