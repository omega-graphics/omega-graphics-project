#include "GTEBase.h"
#include "GTEMath.h"
#include "GE.h"

#ifndef OMEGAGTE_GESPACE_H
#define OMEGAGTE_GESPACE_H

_NAMESPACE_BEGIN_

    /// @brief A coordinate space that places geometry into a viewport-defined
    /// relative space and hands back the transform that maps it to NDC.
    ///
    /// GESpace owns exactly one conversion: relative/world **space units** →
    /// normalized device coordinates, derived from a `GEViewport`. Geometry is
    /// never re-baked on the CPU — a GEMesh's vertex buffer stays in its own
    /// local space, and the composed matrix is what the caller's draw pipeline
    /// applies. Phase 1 supplies the space itself; object placement and
    /// per-object transforms arrive in Phases 2-4 (see
    /// `.plans/GESpace-Implementation-Plan.md`).
    ///
    /// @paragraph Matrix convention. Every matrix GESpace produces is
    /// **column-major**, laid out the way the backends upload it: element
    /// (row r, column c) is `m[c][r]`, translation lives in column 3
    /// (`m[3][0..2]`), and a shader consumes it as `mvp * float4(pos, 1.0)`.
    /// This matches `translationMatrix` / `orthographicProjection` in GTEMath.h
    /// and `Kreate::Mat4`'s `float[16]` layout, so an `FMatrix<4,4>` from here
    /// can be memcpy'd straight into a uniform or push constant.
    class OMEGAGTE_EXPORT GESpace {
    public:
        OMEGACOMMON_CLASS("OmegaGTE.GESpace")

        /// @brief Construct a space whose relative units map to NDC through
        /// `viewport` (origin + width/height + near/far depth). The viewport is
        /// copied by value; call `setViewport()` to re-anchor it (e.g. on a
        /// window resize).
        explicit GESpace(const GEViewport & viewport);
        ~GESpace();

        GESpace(const GESpace &) = delete;
        GESpace & operator=(const GESpace &) = delete;

        /// @brief Re-anchor the space onto a new viewport. Invalidates the
        /// cached space→NDC matrix; the next `spaceToNDC()` recomposes it.
        void setViewport(const GEViewport & viewport);

        /// @brief The viewport this space is currently anchored to.
        const GEViewport & viewport() const;

        /// @brief The linear space→NDC transform derived from the current
        /// viewport. This is GESpace's canonical coordinate conversion.
        ///
        /// It is an origin-aware orthographic map honoring `GEViewport::x` and
        /// `y` (which `viewportMatrix()` in GTEMath.h does not — that one is
        /// scale-only, and is *not* what this uses):
        ///
        ///   x_ndc = 2*(x - vp.x)/vp.width - 1
        ///   y_ndc = 1 - 2*(y - vp.y)/vp.height        (Y-flip: y=0 → +1, the
        ///                                              top-left / Y-down
        ///                                              convention GEViewport
        ///                                              already documents)
        ///   z_ndc = (z - vp.nearDepth)/(vp.farDepth - vp.nearDepth)
        ///
        /// @paragraph Depth range. Z maps to **[0,1]**, the range Vulkan, D3D12
        /// and Metal actually clip against — near→0, far→1. This deliberately
        /// differs from `OmegaTriangulationEngineContext::translateCoords`,
        /// whose `z > 0` branch bakes to the OpenGL-style [-1,1] range; geometry
        /// placed through GESpace is therefore inside the clip volume on every
        /// backend GTE ships, which TE-baked depth is not. X and Y are identical
        /// to `translateCoords` for a viewport anchored at the origin.
        ///
        /// A degenerate viewport (zero width, height, or depth range) has no
        /// meaningful mapping; it logs an error and yields identity rather than
        /// propagating NaNs into a draw call.
        FMatrix<4,4> spaceToNDC() const;

        // Object management (addMesh / transforms / objectTransform) arrives in
        // Phases 2-4.

    private:
        struct Impl;
        UniqueHandle<Impl> impl;
    };

_NAMESPACE_END_

#endif
