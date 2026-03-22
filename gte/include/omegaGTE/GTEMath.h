#ifndef OMEGAGTE_GTEMATH_H
#define OMEGAGTE_GTEMATH_H

#include "GTEBase.h"
#include <cmath>

_NAMESPACE_BEGIN_

    // ==================================================================
    // Constants
    // ==================================================================

    template<class Ty> constexpr Ty Pi       = Ty(3.14159265358979323846);
    template<class Ty> constexpr Ty TwoPi    = Ty(6.28318530717958647692);
    template<class Ty> constexpr Ty HalfPi   = Ty(1.57079632679489661923);
    template<class Ty> constexpr Ty E        = Ty(2.71828182845904523536);
    template<class Ty> constexpr Ty Deg2Rad  = Pi<Ty> / Ty(180);
    template<class Ty> constexpr Ty Rad2Deg  = Ty(180) / Pi<Ty>;

    // ==================================================================
    // Determinant
    // ==================================================================

    template<class Ty>
    inline Ty determinant(const Matrix<Ty,2,2>& m){
        return m[0][0] * m[1][1] - m[0][1] * m[1][0];
    }

    template<class Ty>
    inline Ty determinant(const Matrix<Ty,3,3>& m){
        return m[0][0] * (m[1][1]*m[2][2] - m[1][2]*m[2][1])
             - m[0][1] * (m[1][0]*m[2][2] - m[1][2]*m[2][0])
             + m[0][2] * (m[1][0]*m[2][1] - m[1][1]*m[2][0]);
    }

    template<class Ty>
    inline Ty determinant(const Matrix<Ty,4,4>& m){
        // Laplace expansion along first row using 3x3 cofactors
        Ty s0 = m[0][0]*m[1][1] - m[1][0]*m[0][1];
        Ty s1 = m[0][0]*m[2][1] - m[2][0]*m[0][1];
        Ty s2 = m[0][0]*m[3][1] - m[3][0]*m[0][1];
        Ty s3 = m[1][0]*m[2][1] - m[2][0]*m[1][1];
        Ty s4 = m[1][0]*m[3][1] - m[3][0]*m[1][1];
        Ty s5 = m[2][0]*m[3][1] - m[3][0]*m[2][1];

        Ty c5 = m[2][2]*m[3][3] - m[3][2]*m[2][3];
        Ty c4 = m[1][2]*m[3][3] - m[3][2]*m[1][3];
        Ty c3 = m[1][2]*m[2][3] - m[2][2]*m[1][3];
        Ty c2 = m[0][2]*m[3][3] - m[3][2]*m[0][3];
        Ty c1 = m[0][2]*m[2][3] - m[2][2]*m[0][3];
        Ty c0 = m[0][2]*m[1][3] - m[1][2]*m[0][3];

        return s0*c5 - s1*c4 + s2*c3 + s3*c2 - s4*c1 + s5*c0;
    }

    // ==================================================================
    // Inverse
    // ==================================================================

    template<class Ty>
    inline Matrix<Ty,2,2> inverse(const Matrix<Ty,2,2>& m){
        Ty det = determinant(m);
        Ty invDet = Ty(1) / det;
        auto r = Matrix<Ty,2,2>::Create();
        r[0][0] =  m[1][1] * invDet;
        r[0][1] = -m[0][1] * invDet;
        r[1][0] = -m[1][0] * invDet;
        r[1][1] =  m[0][0] * invDet;
        return r;
    }

    template<class Ty>
    inline Matrix<Ty,3,3> inverse(const Matrix<Ty,3,3>& m){
        Ty det = determinant(m);
        Ty invDet = Ty(1) / det;
        auto r = Matrix<Ty,3,3>::Create();
        r[0][0] = (m[1][1]*m[2][2] - m[1][2]*m[2][1]) * invDet;
        r[0][1] = (m[0][2]*m[2][1] - m[0][1]*m[2][2]) * invDet;
        r[0][2] = (m[0][1]*m[1][2] - m[0][2]*m[1][1]) * invDet;
        r[1][0] = (m[1][2]*m[2][0] - m[1][0]*m[2][2]) * invDet;
        r[1][1] = (m[0][0]*m[2][2] - m[0][2]*m[2][0]) * invDet;
        r[1][2] = (m[0][2]*m[1][0] - m[0][0]*m[1][2]) * invDet;
        r[2][0] = (m[1][0]*m[2][1] - m[1][1]*m[2][0]) * invDet;
        r[2][1] = (m[0][1]*m[2][0] - m[0][0]*m[2][1]) * invDet;
        r[2][2] = (m[0][0]*m[1][1] - m[0][1]*m[1][0]) * invDet;
        return r;
    }

    template<class Ty>
    inline Matrix<Ty,4,4> inverse(const Matrix<Ty,4,4>& m){
        Ty s0 = m[0][0]*m[1][1] - m[1][0]*m[0][1];
        Ty s1 = m[0][0]*m[2][1] - m[2][0]*m[0][1];
        Ty s2 = m[0][0]*m[3][1] - m[3][0]*m[0][1];
        Ty s3 = m[1][0]*m[2][1] - m[2][0]*m[1][1];
        Ty s4 = m[1][0]*m[3][1] - m[3][0]*m[1][1];
        Ty s5 = m[2][0]*m[3][1] - m[3][0]*m[2][1];

        Ty c5 = m[2][2]*m[3][3] - m[3][2]*m[2][3];
        Ty c4 = m[1][2]*m[3][3] - m[3][2]*m[1][3];
        Ty c3 = m[1][2]*m[2][3] - m[2][2]*m[1][3];
        Ty c2 = m[0][2]*m[3][3] - m[3][2]*m[0][3];
        Ty c1 = m[0][2]*m[2][3] - m[2][2]*m[0][3];
        Ty c0 = m[0][2]*m[1][3] - m[1][2]*m[0][3];

        Ty det = s0*c5 - s1*c4 + s2*c3 + s3*c2 - s4*c1 + s5*c0;
        Ty invDet = Ty(1) / det;

        auto r = Matrix<Ty,4,4>::Create();

        r[0][0] = ( m[1][1]*c5 - m[2][1]*c4 + m[3][1]*c3) * invDet;
        r[0][1] = (-m[0][1]*c5 + m[2][1]*c2 - m[3][1]*c1) * invDet;
        r[0][2] = ( m[0][1]*c4 - m[1][1]*c2 + m[3][1]*c0) * invDet;
        r[0][3] = (-m[0][1]*c3 + m[1][1]*c1 - m[2][1]*c0) * invDet;

        r[1][0] = (-m[1][0]*c5 + m[2][0]*c4 - m[3][0]*c3) * invDet;
        r[1][1] = ( m[0][0]*c5 - m[2][0]*c2 + m[3][0]*c1) * invDet;
        r[1][2] = (-m[0][0]*c4 + m[1][0]*c2 - m[3][0]*c0) * invDet;
        r[1][3] = ( m[0][0]*c3 - m[1][0]*c1 + m[2][0]*c0) * invDet;

        r[2][0] = ( m[1][3]*s5 - m[2][3]*s4 + m[3][3]*s3) * invDet;
        r[2][1] = (-m[0][3]*s5 + m[2][3]*s2 - m[3][3]*s1) * invDet;
        r[2][2] = ( m[0][3]*s4 - m[1][3]*s2 + m[3][3]*s0) * invDet;
        r[2][3] = (-m[0][3]*s3 + m[1][3]*s1 - m[2][3]*s0) * invDet;

        r[3][0] = (-m[1][2]*s5 + m[2][2]*s4 - m[3][2]*s3) * invDet;
        r[3][1] = ( m[0][2]*s5 - m[2][2]*s2 + m[3][2]*s1) * invDet;
        r[3][2] = (-m[0][2]*s4 + m[1][2]*s2 - m[3][2]*s0) * invDet;
        r[3][3] = ( m[0][2]*s3 - m[1][2]*s1 + m[2][2]*s0) * invDet;

        return r;
    }

    // ==================================================================
    // Transform matrix builders (float, 4x4)
    // ==================================================================

    inline FMatrix<4,4> translationMatrix(float x, float y, float z){
        auto m = FMatrix<4,4>::Identity();
        m[3][0] = x;
        m[3][1] = y;
        m[3][2] = z;
        return m;
    }

    inline FMatrix<4,4> scalingMatrix(float x, float y, float z){
        auto m = FMatrix<4,4>::Create();
        m[0][0] = x;
        m[1][1] = y;
        m[2][2] = z;
        m[3][3] = 1.f;
        return m;
    }

    inline FMatrix<4,4> rotationX(float radians){
        float c = cosf(radians), s = sinf(radians);
        auto m = FMatrix<4,4>::Identity();
        m[1][1] = c;  m[2][1] = -s;
        m[1][2] = s;  m[2][2] =  c;
        return m;
    }

    inline FMatrix<4,4> rotationY(float radians){
        float c = cosf(radians), s = sinf(radians);
        auto m = FMatrix<4,4>::Identity();
        m[0][0] =  c;  m[2][0] = s;
        m[0][2] = -s;  m[2][2] = c;
        return m;
    }

    inline FMatrix<4,4> rotationZ(float radians){
        float c = cosf(radians), s = sinf(radians);
        auto m = FMatrix<4,4>::Identity();
        m[0][0] = c;  m[1][0] = -s;
        m[0][1] = s;  m[1][1] =  c;
        return m;
    }

    inline FMatrix<4,4> rotationEuler(float pitch, float yaw, float roll){
        return rotationZ(roll) * rotationY(yaw) * rotationX(pitch);
    }

    // ==================================================================
    // Projection / View matrices
    // ==================================================================

    inline FMatrix<4,4> perspectiveProjection(float fovY, float aspect, float nearZ, float farZ){
        float tanHalf = tanf(fovY * 0.5f);
        auto m = FMatrix<4,4>::Create();
        m[0][0] = 1.f / (aspect * tanHalf);
        m[1][1] = 1.f / tanHalf;
        m[2][2] = farZ / (nearZ - farZ);
        m[2][3] = -1.f;
        m[3][2] = (nearZ * farZ) / (nearZ - farZ);
        return m;
    }

    inline FMatrix<4,4> orthographicProjection(float left, float right, float bottom, float top, float nearZ, float farZ){
        auto m = FMatrix<4,4>::Create();
        m[0][0] = 2.f / (right - left);
        m[1][1] = 2.f / (top - bottom);
        m[2][2] = 1.f / (nearZ - farZ);
        m[3][0] = -(right + left) / (right - left);
        m[3][1] = -(top + bottom) / (top - bottom);
        m[3][2] = nearZ / (nearZ - farZ);
        m[3][3] = 1.f;
        return m;
    }

    inline FMatrix<4,4> lookAt(const GPoint3D& eye, const GPoint3D& target, const GPoint3D& up){
        float fx = target.x - eye.x, fy = target.y - eye.y, fz = target.z - eye.z;
        float flen = sqrtf(fx*fx + fy*fy + fz*fz);
        fx /= flen; fy /= flen; fz /= flen;

        // side = normalize(cross(forward, up))
        float sx = fy*up.z - fz*up.y;
        float sy = fz*up.x - fx*up.z;
        float sz = fx*up.y - fy*up.x;
        float slen = sqrtf(sx*sx + sy*sy + sz*sz);
        sx /= slen; sy /= slen; sz /= slen;

        // recomputed up = cross(side, forward)
        float ux = sy*fz - sz*fy;
        float uy = sz*fx - sx*fz;
        float uz = sx*fy - sy*fx;

        auto m = FMatrix<4,4>::Identity();
        m[0][0] = sx;  m[1][0] = sy;  m[2][0] = sz;
        m[0][1] = ux;  m[1][1] = uy;  m[2][1] = uz;
        m[0][2] = -fx; m[1][2] = -fy; m[2][2] = -fz;
        m[3][0] = -(sx*eye.x + sy*eye.y + sz*eye.z);
        m[3][1] = -(ux*eye.x + uy*eye.y + uz*eye.z);
        m[3][2] =  (fx*eye.x + fy*eye.y + fz*eye.z);
        return m;
    }

    // ==================================================================
    // Viewport mapping
    // ==================================================================

    inline FMatrix<4,4> viewportMatrix(float width, float height, float farDepth){
        return scalingMatrix(2.f / width, 2.f / height, 2.f / farDepth);
    }

    // ==================================================================
    // Vector helpers (on FVec<N> = FMatrix<N,1>)
    // ==================================================================

    template<class Ty, unsigned N>
    inline Ty dot(const Matrix<Ty,N,1>& a, const Matrix<Ty,N,1>& b){
        Ty sum = 0;
        for(unsigned i = 0; i < N; i++) sum += a[i][0] * b[i][0];
        return sum;
    }

    template<class Ty>
    inline Matrix<Ty,3,1> cross(const Matrix<Ty,3,1>& a, const Matrix<Ty,3,1>& b){
        auto r = Matrix<Ty,3,1>::Create();
        r[0][0] = a[1][0]*b[2][0] - a[2][0]*b[1][0];
        r[1][0] = a[2][0]*b[0][0] - a[0][0]*b[2][0];
        r[2][0] = a[0][0]*b[1][0] - a[1][0]*b[0][0];
        return r;
    }

    template<class Ty, unsigned N>
    inline Ty length(const Matrix<Ty,N,1>& v){
        return std::sqrt(dot(v,v));
    }

    template<class Ty, unsigned N>
    inline Matrix<Ty,N,1> normalize(const Matrix<Ty,N,1>& v){
        Ty len = length(v);
        return v * (Ty(1) / len);
    }

    // ==================================================================
    // Point ↔ Matrix helpers
    // ==================================================================

    inline FVec<4> pointToVec4(const GPoint3D& pt, float w = 1.f){
        auto v = FVec<4>::Create();
        v[0][0] = pt.x; v[1][0] = pt.y; v[2][0] = pt.z; v[3][0] = w;
        return v;
    }

    inline GPoint3D vec4ToPoint(const FVec<4>& v){
        return GPoint3D{v[0][0], v[1][0], v[2][0]};
    }

    inline GPoint3D transformPoint(const FMatrix<4,4>& m, const GPoint3D& pt){
        auto v = m * pointToVec4(pt);
        return vec4ToPoint(v);
    }

_NAMESPACE_END_

#endif
