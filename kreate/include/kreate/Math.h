#ifndef KREATE_MATH_H
#define KREATE_MATH_H

#include "Base.h"

namespace Kreate {

struct KREATE_EXPORT Vec3 {
    float x = 0.f, y = 0.f, z = 0.f;
};

struct KREATE_EXPORT Vec4 {
    float x = 0.f, y = 0.f, z = 0.f, w = 0.f;
};

struct KREATE_EXPORT Color {
    float r = 0.f, g = 0.f, b = 0.f, a = 1.f;
};

/// Row-major 4x4 matrix: `data[row*4 + col]` is element (row, col), and
/// `operator*` is a standard mathematical product — `A * B` means the logical
/// matrix A·B, so `projection * view * world` reads left to right and, applied
/// to a column vector, transforms world first, then view, then projection.
///
/// GTE's `FMatrix<4,4>` is column-major AND its `operator*` composes in reverse
/// (see GESpace-Implementation-Plan Finding A). Keeping Kreate's own math
/// conventional and isolating that quirk to a single transpose at the GTE
/// boundary (Math.cpp's `toFMatrix` / `copyFromFMatrix`, and the renderer's
/// push-constant flatten) is what makes the composition order come out right.
/// A row-major store transposed into a column-major store IS the same logical
/// matrix, so the boundary transpose changes storage, not meaning.
struct KREATE_EXPORT Mat4 {
    float data[16];

    static Mat4 identity();
    static Mat4 perspective(float fovYRadians, float aspect, float nearZ, float farZ);
    static Mat4 lookAt(Vec3 eye, Vec3 target, Vec3 up);
    static Mat4 translation(Vec3 t);
    static Mat4 rotation(float angleRadians, Vec3 axis);
    static Mat4 scale(Vec3 s);

    Mat4 operator*(const Mat4 &rhs) const;
};

} // namespace Kreate

#endif // KREATE_MATH_H
