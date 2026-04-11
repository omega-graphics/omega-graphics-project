#ifndef OMEGAWTK_COMPOSITION_GTEFORWARD_H
#define OMEGAWTK_COMPOSITION_GTEFORWARD_H

namespace OmegaGTE {
    struct GPoint2D;
    struct GEllipsoid;
    template<class Pt_Ty> class GVectorPath_Base;
    typedef GVectorPath_Base<GPoint2D> GVectorPath2D;
    template<class Ty, unsigned column, unsigned row> class Matrix;
    template<unsigned c, unsigned r> using FMatrix = Matrix<float, c, r>;
    class GETexture;
    class GEFence;
}

#endif
