#include "../VisualTree.h"
#include "../RenderTarget.h"

#include "NativePrivate/gtk/GTKItem.h"


#ifndef OMEGAWTK_COMPOSITION_VK_VKLAYERTREE_H
#define OMEGAWTK_COMPOSITION_VK_VKLAYERTREE_H

namespace OmegaWTK::Composition {

    class VKLayerTree : public BackendVisualTree {
        typedef BackendVisualTree Parent;
        SharedHandle<Native::GTK::GTKItem> view;
    protected:
        using Parent::body;
        using Parent::root;
    };

    
}

#endif