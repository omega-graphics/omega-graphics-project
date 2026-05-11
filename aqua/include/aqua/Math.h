#ifndef AQUA_MATH_H
#define AQUA_MATH_H

#include "Base.h"

namespace Aqua {

struct AQUA_EXPORT Vec3 {
    float x = 0.f, y = 0.f, z = 0.f;
};

struct AQUA_EXPORT Vec4 {
    float x = 0.f, y = 0.f, z = 0.f, w = 0.f;
};

struct AQUA_EXPORT Color {
    float r = 0.f, g = 0.f, b = 0.f, a = 1.f;
};

/// Column-major 4x4 matrix. `data[col*4 + row]` matches the storage order
/// expected by GTE's `FMatrix<4,4>`, so the impl in Math.cpp can memcpy
/// rows out without transposing.
struct AQUA_EXPORT Mat4 {
    float data[16];

    static Mat4 identity();
    static Mat4 perspective(float fovYRadians, float aspect, float nearZ, float farZ);
    static Mat4 lookAt(Vec3 eye, Vec3 target, Vec3 up);
    static Mat4 translation(Vec3 t);
    static Mat4 rotation(float angleRadians, Vec3 axis);
    static Mat4 scale(Vec3 s);

    Mat4 operator*(const Mat4 &rhs) const;
};

} // namespace Aqua

#endif // AQUA_MATH_H
