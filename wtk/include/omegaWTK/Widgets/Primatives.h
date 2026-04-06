#include "omegaWTK/UI/Widget.h"

#ifndef OMEGAWTK_WIDGETS_PRIMATIVES_H
#define OMEGAWTK_WIDGETS_PRIMATIVES_H

namespace OmegaWTK {
 // @note We Should use OmegaCommon::UString string for all text widgets. (Unicode support for rendering all languages.)
    class OMEGAWTK_EXPORT Label : public Widget {
    public:
        WIDGET_CONSTRUCTOR()
    };

    class OMEGAWTK_EXPORT Icon : public Widget {
    public:
        WIDGET_CONSTRUCTOR()
    };

    class OMEGAWTK_EXPORT Image : public Widget {
    public:
        WIDGET_CONSTRUCTOR()
    };

};
#endif //OMEGAWTK_WIDGETS_PRIMATIVES_H
