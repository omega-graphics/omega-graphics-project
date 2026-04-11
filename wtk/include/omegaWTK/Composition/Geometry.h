#ifndef OMEGAWTK_COMPOSITION_GEOMETRY_H
#define OMEGAWTK_COMPOSITION_GEOMETRY_H

#include <cstring>

namespace OmegaWTK::Composition {

    struct Point2D {
        float x, y;
    };

    struct Rect {
        Point2D pos;
        float w, h;
    };

    struct RoundedRect {
        Point2D pos;
        float w, h, rad_x, rad_y;
    };

    struct Ellipse {
        float x, y, rad_x, rad_y;
    };

    struct Matrix4x4 {
        float m[16];

        static Matrix4x4 Identity() {
            Matrix4x4 mat;
            std::memset(mat.m, 0, sizeof(mat.m));
            mat.m[0]  = 1.f;
            mat.m[5]  = 1.f;
            mat.m[10] = 1.f;
            mat.m[15] = 1.f;
            return mat;
        }

        const float* data() const { return m; }
        float* data() { return m; }
    };

}

#endif
