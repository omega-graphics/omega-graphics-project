#ifndef AQUA_AQMATH_H
#define AQUA_AQMATH_H

#include "AQBase.h"

/// Minimal 3-vector for the physics public surface. Deliberately independent of
/// any graphics-engine math type so AQUA carries no link-time dependency on a
/// renderer — kREATE converts between this and its own `Kreate::Vec3` at the
/// integration boundary. Kept an aggregate, like `Kreate::Vec3`; construct with
/// brace initialization, e.g. `Vec3{0.f, 10.f, 0.f}`.
struct AQUA_EXPORT Vec3 {
    float x = 0.f, y = 0.f, z = 0.f;

    Vec3 operator+(const Vec3 &o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3 &o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 &operator+=(const Vec3 &o) { x += o.x; y += o.y; z += o.z; return *this; }
};

#endif // AQUA_AQMATH_H
