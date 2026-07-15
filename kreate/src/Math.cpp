#include <kreate/Math.h>
#include "MathConvert.h"
#include <omegaGTE/GTEMath.h>
#include <cmath>
#include <cstring>

namespace Kreate {

namespace {

// GTE's FMatrix is column-major (`m[c][r]` is element (row r, col c)); Kreate's
// Mat4 is row-major (`data[r*4 + c]`). The two stores of one logical matrix are
// transposes of each other, so the conversion maps `m[c][r] <-> data[r*4 + c]`.

inline void copyFromFMatrix(const OmegaGTE::FMatrix<4,4> &m, float out[16]) {
    for (unsigned r = 0; r < 4; ++r) {
        for (unsigned c = 0; c < 4; ++c) {
            out[r * 4 + c] = m[c][r];
        }
    }
}

} // namespace

OmegaGTE::FMatrix<4,4> toFMatrix(const Mat4 &m) {
    auto out = OmegaGTE::FMatrix<4,4>::Create();
    for (unsigned r = 0; r < 4; ++r) {
        for (unsigned c = 0; c < 4; ++c) {
            out[c][r] = m.data[r * 4 + c];
        }
    }
    return out;
}

Mat4 Mat4::identity() {
    Mat4 m{};
    m.data[0] = 1.f; m.data[5] = 1.f; m.data[10] = 1.f; m.data[15] = 1.f;
    return m;
}

Mat4 Mat4::perspective(float fovYRadians, float aspect, float nearZ, float farZ) {
    Mat4 r{};
    copyFromFMatrix(OmegaGTE::perspectiveProjection(fovYRadians, aspect, nearZ, farZ), r.data);
    return r;
}

Mat4 Mat4::lookAt(Vec3 eye, Vec3 target, Vec3 up) {
    Mat4 r{};
    OmegaGTE::GPoint3D e{eye.x, eye.y, eye.z};
    OmegaGTE::GPoint3D t{target.x, target.y, target.z};
    OmegaGTE::GPoint3D u{up.x, up.y, up.z};
    copyFromFMatrix(OmegaGTE::lookAt(e, t, u), r.data);
    return r;
}

Mat4 Mat4::translation(Vec3 t) {
    Mat4 r{};
    copyFromFMatrix(OmegaGTE::translationMatrix(t.x, t.y, t.z), r.data);
    return r;
}

Mat4 Mat4::rotation(float angleRadians, Vec3 axis) {
    // GTE doesn't expose a generic axis-angle matrix builder, but the
    // quaternion path does. Convert axis-angle -> quaternion -> matrix.
    float len = std::sqrt(axis.x*axis.x + axis.y*axis.y + axis.z*axis.z);
    if (len <= 0.f) return Mat4::identity();
    float ix = axis.x / len, iy = axis.y / len, iz = axis.z / len;
    auto q = OmegaGTE::FQuaternion::fromAxisAngle(ix, iy, iz, angleRadians);
    Mat4 r{};
    copyFromFMatrix(q.toMatrix(), r.data);
    return r;
}

Mat4 Mat4::scale(Vec3 s) {
    Mat4 r{};
    copyFromFMatrix(OmegaGTE::scalingMatrix(s.x, s.y, s.z), r.data);
    return r;
}

Mat4 Mat4::operator*(const Mat4 &rhs) const {
    // Standard row-major product: out = this · rhs (logical A·B). Computed
    // directly rather than through GTE's operator*, which composes in reverse
    // (GESpace-Implementation-Plan Finding A) — routing through it is exactly
    // what silently flipped `projection * view * world` before this fix.
    Mat4 out{};
    for (unsigned r = 0; r < 4; ++r) {
        for (unsigned c = 0; c < 4; ++c) {
            float sum = 0.f;
            for (unsigned k = 0; k < 4; ++k) {
                sum += data[r * 4 + k] * rhs.data[k * 4 + c];
            }
            out.data[r * 4 + c] = sum;
        }
    }
    return out;
}

} // namespace Kreate
