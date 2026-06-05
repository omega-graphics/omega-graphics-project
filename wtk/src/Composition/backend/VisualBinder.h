#ifndef OMEGAWTK_COMPOSITION_VISUALBINDER_H
#define OMEGAWTK_COMPOSITION_VISUALBINDER_H

#include "omegaWTK/Native/NativeVisualTree.h"
#include "RenderTarget.h"

#include <memory>

namespace OmegaWTK::Composition {

    /// Per-backend "build an RTC for the tree's root visual" (§2.14
    /// Pass 1). Returns nullptr on pre-realize (Linux X11 Window not
    /// yet live); the compositor retries on each frame until the bind
    /// succeeds, mirroring the pre-§2.14 `resolveDeferredNativeTarget`
    /// retry loop.
    ///
    /// The per-platform definition lives in the matching backend
    /// directory — `backend/vk/VKVisualBinder.cpp`,
    /// `backend/mtl/MTLVisualBinder.mm`,
    /// `backend/dx/DCVisualBinder.cpp`. Only one is linked into any
    /// single build (matches the per-backend `make_native_visual_tree`
    /// factory). The cpp downcasts the abstract `Native::VisualTree &`
    /// to its concrete subclass (`Native::GTK::VKVisualTree` etc.) to
    /// read platform-specific handles.
    std::unique_ptr<BackendRenderTargetContext>
    tryBindRootVisual(Native::VisualTree & tree);

}

#endif
