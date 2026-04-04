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

    // ==================================================================
    // Quaternion
    // ==================================================================

    template<class Ty>
    struct Quaternion {
        Ty x, y, z, w;

        // --- Construction ---

        /// Returns the identity quaternion (0, 0, 0, 1).
        static Quaternion Identity(){
            return {Ty(0), Ty(0), Ty(0), Ty(1)};
        }

        /// Rotation of `radians` around the unit axis (ax, ay, az).
        static Quaternion fromAxisAngle(Ty ax, Ty ay, Ty az, Ty radians){
            Ty half = radians * Ty(0.5);
            Ty s = std::sin(half);
            return {ax * s, ay * s, az * s, std::cos(half)};
        }

        /// Equivalent to rotationEuler() but as a quaternion.
        /// Applies X (pitch) -> Y (yaw) -> Z (roll), matching the existing convention.
        static Quaternion fromEuler(Ty pitch, Ty yaw, Ty roll){
            Ty cx = std::cos(pitch * Ty(0.5)), sx = std::sin(pitch * Ty(0.5));
            Ty cy = std::cos(yaw   * Ty(0.5)), sy = std::sin(yaw   * Ty(0.5));
            Ty cz = std::cos(roll  * Ty(0.5)), sz = std::sin(roll  * Ty(0.5));
            return {
                sx*cy*cz - cx*sy*sz,
                cx*sy*cz + sx*cy*sz,
                cx*cy*sz - sx*sy*cz,
                cx*cy*cz + sx*sy*sz
            };
        }

        /// Extracts the rotation from a 4x4 matrix (upper-left 3x3).
        /// Uses Shepperd's method for numerical stability.
        static Quaternion fromMatrix(const Matrix<Ty,4,4>& m){
            Ty m00 = m[0][0], m11 = m[1][1], m22 = m[2][2];
            Ty trace = m00 + m11 + m22;
            Ty qx, qy, qz, qw;

            if(trace > Ty(0)){
                Ty s = std::sqrt(trace + Ty(1)) * Ty(2); // s = 4*w
                qw = Ty(0.25) * s;
                qx = (m[1][2] - m[2][1]) / s;
                qy = (m[2][0] - m[0][2]) / s;
                qz = (m[0][1] - m[1][0]) / s;
            } else if(m00 > m11 && m00 > m22){
                Ty s = std::sqrt(Ty(1) + m00 - m11 - m22) * Ty(2); // s = 4*x
                qw = (m[1][2] - m[2][1]) / s;
                qx = Ty(0.25) * s;
                qy = (m[0][1] + m[1][0]) / s;
                qz = (m[2][0] + m[0][2]) / s;
            } else if(m11 > m22){
                Ty s = std::sqrt(Ty(1) + m11 - m00 - m22) * Ty(2); // s = 4*y
                qw = (m[2][0] - m[0][2]) / s;
                qx = (m[0][1] + m[1][0]) / s;
                qy = Ty(0.25) * s;
                qz = (m[1][2] + m[2][1]) / s;
            } else {
                Ty s = std::sqrt(Ty(1) + m22 - m00 - m11) * Ty(2); // s = 4*z
                qw = (m[0][1] - m[1][0]) / s;
                qx = (m[2][0] + m[0][2]) / s;
                qy = (m[1][2] + m[2][1]) / s;
                qz = Ty(0.25) * s;
            }
            return {qx, qy, qz, qw};
        }

        // --- Arithmetic ---

        /// Hamilton product. Composes rotations: (q1 * q2) applies q2 first, then q1.
        Quaternion operator*(const Quaternion& o) const {
            return {
                w*o.x + x*o.w + y*o.z - z*o.y,
                w*o.y - x*o.z + y*o.w + z*o.x,
                w*o.z + x*o.y - y*o.x + z*o.w,
                w*o.w - x*o.x - y*o.y - z*o.z
            };
        }

        Quaternion operator*(Ty scalar) const {
            return {x*scalar, y*scalar, z*scalar, w*scalar};
        }
        friend Quaternion operator*(Ty scalar, const Quaternion& q){
            return q * scalar;
        }

        Quaternion operator+(const Quaternion& o) const {
            return {x+o.x, y+o.y, z+o.z, w+o.w};
        }
        Quaternion operator-(const Quaternion& o) const {
            return {x-o.x, y-o.y, z-o.z, w-o.w};
        }
        Quaternion operator-() const {
            return {-x, -y, -z, -w};
        }

        // --- Operations ---

        Ty lengthSquared() const { return x*x + y*y + z*z + w*w; }
        Ty length() const { return std::sqrt(lengthSquared()); }

        Quaternion normalized() const {
            Ty len = length();
            return {x/len, y/len, z/len, w/len};
        }

        Quaternion conjugate() const { return {-x, -y, -z, w}; }

        Quaternion inverse() const {
            Ty lenSq = lengthSquared();
            return {-x/lenSq, -y/lenSq, -z/lenSq, w/lenSq};
        }

        Ty dot(const Quaternion& o) const {
            return x*o.x + y*o.y + z*o.z + w*o.w;
        }

        // --- Conversion ---

        /// Produces a 4x4 rotation matrix (no translation/scale).
        Matrix<Ty,4,4> toMatrix() const {
            Ty xx = x*x, yy = y*y, zz = z*z;
            Ty xy = x*y, xz = x*z, yz = y*z;
            Ty wx = w*x, wy = w*y, wz = w*z;

            auto m = Matrix<Ty,4,4>::Create();
            m[0][0] = Ty(1) - Ty(2)*(yy + zz);
            m[0][1] = Ty(2)*(xy + wz);
            m[0][2] = Ty(2)*(xz - wy);

            m[1][0] = Ty(2)*(xy - wz);
            m[1][1] = Ty(1) - Ty(2)*(xx + zz);
            m[1][2] = Ty(2)*(yz + wx);

            m[2][0] = Ty(2)*(xz + wy);
            m[2][1] = Ty(2)*(yz - wx);
            m[2][2] = Ty(1) - Ty(2)*(xx + yy);

            m[3][3] = Ty(1);
            return m;
        }

        // --- Interpolation ---

        /// Normalized linear interpolation. Cheaper than slerp, nearly identical
        /// for small angular differences.
        static Quaternion nlerp(const Quaternion& a, const Quaternion& b, Ty t){
            Quaternion target = (a.dot(b) < Ty(0)) ? -b : b;
            return (a * (Ty(1) - t) + target * t).normalized();
        }

        /// Spherical linear interpolation. t=0 returns a, t=1 returns b.
        /// Follows the shortest arc (flips b if dot(a,b) < 0).
        static Quaternion slerp(const Quaternion& a, const Quaternion& b, Ty t){
            Ty cosTheta = a.dot(b);
            Quaternion target = b;
            if(cosTheta < Ty(0)){
                target = -b;
                cosTheta = -cosTheta;
            }
            if(cosTheta > Ty(0.9995)){
                return nlerp(a, target, t);
            }
            Ty theta = std::acos(cosTheta);
            Ty sinTheta = std::sin(theta);
            Ty wa = std::sin((Ty(1) - t) * theta) / sinTheta;
            Ty wb = std::sin(t * theta) / sinTheta;
            return a * wa + target * wb;
        }
    };

    using FQuaternion = Quaternion<float>;

    // ==================================================================
    // Quaternion free functions
    // ==================================================================

    /// Rotate a point by a quaternion (optimized q * p * q_inverse).
    template<class Ty>
    inline GPoint3D rotatePoint(const Quaternion<Ty>& q, const GPoint3D& pt){
        // t = 2 * cross(q.xyz, pt)
        Ty tx = Ty(2) * (q.y * pt.z - q.z * pt.y);
        Ty ty = Ty(2) * (q.z * pt.x - q.x * pt.z);
        Ty tz = Ty(2) * (q.x * pt.y - q.y * pt.x);
        // result = pt + w*t + cross(q.xyz, t)
        return {
            pt.x + q.w * tx + (q.y * tz - q.z * ty),
            pt.y + q.w * ty + (q.z * tx - q.x * tz),
            pt.z + q.w * tz + (q.x * ty - q.y * tx)
        };
    }

    /// Build a lookAt-style quaternion (camera orientation).
    inline FQuaternion lookAtQuaternion(const GPoint3D& forward, const GPoint3D& up){
        // Normalize forward
        float flen = std::sqrt(forward.x*forward.x + forward.y*forward.y + forward.z*forward.z);
        float fx = forward.x/flen, fy = forward.y/flen, fz = forward.z/flen;

        // side = normalize(cross(forward, up))
        float sx = fy*up.z - fz*up.y;
        float sy = fz*up.x - fx*up.z;
        float sz = fx*up.y - fy*up.x;
        float slen = std::sqrt(sx*sx + sy*sy + sz*sz);
        sx /= slen; sy /= slen; sz /= slen;

        // recomputed up = cross(side, forward)
        float ux = sy*fz - sz*fy;
        float uy = sz*fx - sx*fz;
        float uz = sx*fy - sy*fx;

        // Build 3x3 rotation matrix and extract quaternion
        // Row 0: side,  Row 1: up,  Row 2: -forward
        auto m = FMatrix<4,4>::Identity();
        m[0][0] = sx;  m[0][1] = ux;  m[0][2] = -fx;
        m[1][0] = sy;  m[1][1] = uy;  m[1][2] = -fy;
        m[2][0] = sz;  m[2][1] = uz;  m[2][2] = -fz;
        return FQuaternion::fromMatrix(m);
    }

_NAMESPACE_END_

#endif
