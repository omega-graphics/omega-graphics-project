#include "omegaWTK/Core/Core.h"

#ifndef OMEGAWTK_COMPOSITION_CANVASEFFECT_H
#define OMEGAWTK_COMPOSITION_CANVASEFFECT_H

namespace OmegaWTK::Composition {

    /// Blur effect descriptor. Tier 4 §4.2 rehomed this out of the
    /// deleted `Canvas.h` *unchanged* — it is still consumed by the GPU
    /// blur pipeline (`BackendCanvasEffectProcessor` + the per-platform
    /// processors) and by the UIView style layer
    /// (`Style::elementGaussianBlur` / `elementDirectionalBlur`,
    /// `ResolvedEffectStyle`). The proper model — effects as a
    /// *layer-based* concept rather than a Canvas-adjacent one — is a
    /// future phase (see UIView-Render-Redesign-Plan "Phase E —
    /// Layer-based effects"); this header is the transitional home until
    /// then.
    struct OMEGAWTK_EXPORT CanvasEffect {
        enum class Type : OPT_PARAM {
            DirectionalBlur,
            GaussianBlur
        };
        struct GaussianBlurParams {
            float radius = 0.F;
        };
        struct DirectionalBlurParams {
            float radius = 0.F;
            float angle = 0.F;
        };
        Type type = Type::GaussianBlur;
        /// Legacy optional payload pointer. Prefer gaussianBlur/directionalBlur owned fields.
        void *params = nullptr;
        GaussianBlurParams gaussianBlur {};
        DirectionalBlurParams directionalBlur {};
    };

}

#endif
