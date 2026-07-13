#include "omegaGTE/GESpace.h"

#include <cmath>
#include <iostream>

_NAMESPACE_BEGIN_

namespace {

    bool viewportIsDegenerate(const GEViewport & vp){
        const float depthRange = vp.farDepth - vp.nearDepth;
        return vp.width == 0.f || vp.height == 0.f || depthRange == 0.f;
    }

    /// Compose the origin-aware space→NDC map for `vp`.
    ///
    /// X and Y come straight from `orthographicProjection`, which already emits
    /// exactly the mapping GESpace wants: passing the viewport's bottom edge as
    /// `bottom` and its top edge as `top` (i.e. swapped, since GEViewport is
    /// Y-down) encodes the Y-flip, and its `m[3][0]` / `m[3][1]` terms carry the
    /// viewport origin. Depth is the one row it cannot supply: its Z maps
    /// near→0 but far→**-1**, so this overwrites column 2 / column 3 row 2 with
    /// the [0,1] range the backends clip against.
    FMatrix<4,4> composeSpaceToNDC(const GEViewport & vp){
        auto m = orthographicProjection(vp.x, vp.x + vp.width,      // left,  right
                                        vp.y + vp.height, vp.y,     // bottom, top (Y-flip)
                                        vp.nearDepth, vp.farDepth);

        // z_ndc = (z - near) / (far - near), written column-major: the row-2
        // terms of a column-major matrix are m[2][2] (the z coefficient) and
        // m[3][2] (the constant), so z_ndc = m[2][2]*z + m[3][2].
        const float depthRange = vp.farDepth - vp.nearDepth;
        m[2][2] = 1.f / depthRange;
        m[3][2] = -vp.nearDepth / depthRange;
        return m;
    }

}  // namespace

struct GESpace::Impl {
    GEViewport viewport;
    FMatrix<4,4> spaceToNDC = FMatrix<4,4>::Identity();

    explicit Impl(const GEViewport & vp):viewport(vp){
        recompose();
    }

    void recompose(){
        if(viewportIsDegenerate(viewport)){
            std::cerr << "[GESpace] error: degenerate viewport (width=" << viewport.width
                      << ", height=" << viewport.height
                      << ", depth range=" << (viewport.farDepth - viewport.nearDepth)
                      << "); space->NDC is identity until a valid viewport is set."
                      << std::endl;
            spaceToNDC = FMatrix<4,4>::Identity();
            return;
        }
        spaceToNDC = composeSpaceToNDC(viewport);
    }
};

GESpace::GESpace(const GEViewport & viewport):impl(std::make_unique<Impl>(viewport)){

}

GESpace::~GESpace() = default;

void GESpace::setViewport(const GEViewport & viewport){
    impl->viewport = viewport;
    impl->recompose();
}

const GEViewport & GESpace::viewport() const {
    return impl->viewport;
}

FMatrix<4,4> GESpace::spaceToNDC() const {
    return impl->spaceToNDC;
}

_NAMESPACE_END_
