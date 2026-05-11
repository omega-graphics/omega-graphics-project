#include <aqua/Math.h>
#include <OmegaGTE.h>
#include <omegaGTE/GTEMath.h>
#include <cmath>
#include <cstring>

namespace Aqua {

namespace {

inline void copyFromFMatrix(const OmegaGTE::FMatrix<4,4> &m, float out[16]) {
    for (unsigned c = 0; c < 4; ++c) {
        for (unsigned r = 0; r < 4; ++r) {
            out[c * 4 + r] = m[c][r];
        }
    }
}

inline OmegaGTE::FMatrix<4,4> toFMatrix(const float in[16]) {
    auto m = OmegaGTE::FMatrix<4,4>::Create();
    for (unsigned c = 0; c < 4; ++c) {
        for (unsigned r = 0; r < 4; ++r) {
            m[c][r] = in[c * 4 + r];
        }
    }
    return m;
}

} // namespace

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
    auto a = toFMatrix(data);
    auto b = toFMatrix(rhs.data);
    Mat4 out{};
    copyFromFMatrix(a * b, out.data);
    return out;
}

} // namespace Aqua
