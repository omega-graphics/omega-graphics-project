#ifndef AQUA_AQMATH_H
#define AQUA_AQMATH_H

// AQUA-owned rigid-body math, built on the borrowed OmegaGTE Matrix/Quaternion
// (the deliberate math exception to the backend-hiding rule — see AGENTS.md and
// Phase-1-Dynamics-Math-Core.md §7). Everything here is `Ty`-generic so the
// solver can instantiate `double` as a CPU reference oracle and `float` for the
// production/GPU path from the *same* code (Phase 1 doc §5, §6 parity harness).
//
// Naming follows AGENTS.md: no namespace, every public name carries the `AQ`
// prefix. (The Phase-1 doc drafted these in `namespace aqua`; the house style in
// AGENTS.md governs, so they are free `AQ`-prefixed templates instead.)
//
// Why these exist: GTE ships Matrix/Quaternion and analytic inverse/cross/dot,
// but has no quaternion exp/log map, no "integrate orientation from angular
// velocity", no rotate-free-vector (only rotatePoint for GPoint3D), no
// skew/cross-product matrix, and no inertia builders. Those are physics types we
// own; this header fills exactly that gap.

#include <omegaGTE/GTEMath.h>
#include <cmath>
#include <limits>

// --- Convenience aliases. A "Vec3" in this engine *is* a GTE column vector. ---
// Templated, so they cannot collapse to a single non-template `Vec3` alias —
// that is also why AQSpace.cpp uses OmegaGTE::FVec<3> directly rather than a
// bare `Vec3` (see the folded-in Phase 0 fix, Phase-1 doc §10).
template<class Ty> using AQVec3 = OmegaGTE::Matrix<Ty,3,1>;
template<class Ty> using AQMat3 = OmegaGTE::Matrix<Ty,3,3>;
using AQMat3F = AQMat3<float>;     // public-API mass-property tensor form

// --- Ergonomic construction. GTE's Matrix has a *private* default ctor and no
// component constructor (named-ctor idiom — Create()/Identity()), so `{x,y,z}`
// init does not work; this gives it back without modifying GTE. ---
template<class Ty>
inline AQVec3<Ty> AQvec3(Ty x, Ty y, Ty z) {
    auto v = AQVec3<Ty>::Create();
    v[0][0] = x; v[1][0] = y; v[2][0] = z;
    return v;
}

// --- cross-product (skew-symmetric) matrix:  AQskew(a) * b == cross(a, b) ---
// GTE's 3x3 multiply treats m[i][k] as the (row i, col k) entry; FVec component
// i is v[i][0]. The layout below is verified against OmegaGTE::cross.
template<class Ty>
inline AQMat3<Ty> AQskew(const AQVec3<Ty>& a) {
    const Ty x = a[0][0], y = a[1][0], z = a[2][0];
    auto m = AQMat3<Ty>::Create();
    m[0][1] = -z; m[0][2] =  y;
    m[1][0] =  z; m[1][2] = -x;
    m[2][0] = -y; m[2][1] =  x;
    return m;
}

// --- exponential map: quaternion from a half-angle rotation vector (½·φ) ---
// Uses the sinc Taylor series near 0 so it is finite at zero rotation and the
// C++ and (future) OmegaSL paths agree (no raw libm sin/cos 0/0 divergence).
// Unit to O(t^4). This is why we do *not* reuse Quaternion::fromAxisAngle, which
// is raw sin/cos and not small-angle stable.
template<class Ty>
inline OmegaGTE::Quaternion<Ty> AQquatExp(const AQVec3<Ty>& halfAngle) {
    const Ty x = halfAngle[0][0], y = halfAngle[1][0], z = halfAngle[2][0];
    const Ty t2 = x*x + y*y + z*z;          // |½·φ|^2
    const Ty t  = std::sqrt(t2);
    const Ty s  = (t < Ty(1e-4)) ? (Ty(1) - t2 / Ty(6)) : (std::sin(t) / t);  // sinc
    const Ty w  = (t < Ty(1e-4)) ? (Ty(1) - t2 / Ty(2)) :  std::cos(t);
    return { x*s, y*s, z*s, w };
}

// --- inverse map: half-angle rotation vector from a (unit) quaternion ---
// Inverse of AQquatExp: returns the ½·φ vector v such that AQquatExp(v) ≈ q.
// Small-angle stable — falls back to v ≈ xyz / w when the vector part is tiny.
template<class Ty>
inline AQVec3<Ty> AQquatLog(const OmegaGTE::Quaternion<Ty>& q) {
    const Ty s2 = q.x*q.x + q.y*q.y + q.z*q.z;   // |xyz|^2
    const Ty s  = std::sqrt(s2);
    Ty scale;
    if (s < Ty(1e-4)) {
        // t = atan2(s, w) ≈ s/w (− ...) for small s; v = xyz · (t/s) ≈ xyz/w.
        scale = (q.w != Ty(0)) ? (Ty(1) / q.w) : Ty(0);
    } else {
        scale = std::atan2(s, q.w) / s;          // t / s
    }
    return AQvec3<Ty>(q.x * scale, q.y * scale, q.z * scale);
}

// --- integrate orientation by BODY-frame angular velocity over dt (§6 step 4)
// --- right-multiply (body frame); .normalized() is the belt-and-suspenders
// guard, not load-bearing (exp-map already keeps q on the unit sphere).
template<class Ty>
inline OmegaGTE::Quaternion<Ty> AQintegrate(const OmegaGTE::Quaternion<Ty>& q,
                                            const AQVec3<Ty>& omegaBody, Ty dt) {
    return (q * AQquatExp(omegaBody * (dt * Ty(0.5)))).normalized();
}

// --- rotate a FREE vector (GTE only ships rotatePoint() for GPoint3D) ---
// v' = v + 2w(u×v) + 2u×(u×v), with u = q.xyz. No quaternion inverse needed.
template<class Ty>
inline AQVec3<Ty> AQrotate(const OmegaGTE::Quaternion<Ty>& q, const AQVec3<Ty>& v) {
    auto u = AQvec3<Ty>(q.x, q.y, q.z);
    const auto t = Ty(2) * OmegaGTE::cross(u, v);
    return v + q.w * t + OmegaGTE::cross(u, t);
}

// ====================================================================
// Inertia: diagonal principal moments (the body-frame Ib of the §6 state)
// ====================================================================

// Solid box, half-extents (hx, hy, hz). I_x = (m/3)(hy² + hz²), etc.
template<class Ty>
inline AQVec3<Ty> AQinertiaSolidBox(Ty mass, Ty hx, Ty hy, Ty hz) {
    const Ty c = mass / Ty(3);
    return AQvec3<Ty>(c * (hy*hy + hz*hz),
                      c * (hx*hx + hz*hz),
                      c * (hx*hx + hy*hy));
}

// Solid sphere, radius r. I = (2/5) m r² on every axis.
template<class Ty>
inline AQVec3<Ty> AQinertiaSolidSphere(Ty mass, Ty r) {
    const Ty i = Ty(2) / Ty(5) * mass * r * r;
    return AQvec3<Ty>(i, i, i);
}

// Solid capsule aligned on the local Y axis: a cylinder of half-height h and
// radius r capped by two hemispheres of radius r. Mass is split between the
// parts by volume; moments combine via the parallel-axis theorem for the caps.
template<class Ty>
inline AQVec3<Ty> AQinertiaCapsule(Ty mass, Ty r, Ty h) {
    const Ty r2 = r * r;
    const Ty cylH = Ty(2) * h;                       // full cylinder height
    const Ty volCyl  = Ty(3.14159265358979323846) * r2 * cylH;
    const Ty volCap  = Ty(2) / Ty(3) * Ty(3.14159265358979323846) * r2 * r; // both hemispheres = one sphere
    const Ty total   = volCyl + volCap;
    const Ty mCyl = mass * (volCyl / total);
    const Ty mCap = mass * (volCap / total);

    // Cylinder about its central axis (Y) and transverse axes.
    const Ty cylY = Ty(0.5) * mCyl * r2;
    const Ty cylT = mCyl * (Ty(3) * r2 + cylH * cylH) / Ty(12);

    // Two hemispheres ≡ one sphere. Axial: (2/5) m r². Transverse picks up the
    // parallel-axis shift of each hemisphere's centroid from the body centre.
    const Ty capY = Ty(2) / Ty(5) * mCap * r2;
    const Ty d    = h + Ty(3) * r / Ty(8);           // hemisphere centroid offset from centre
    const Ty capT = capY + mCap * d * d;             // (sphere transverse == axial) + shift

    const Ty iY = cylY + capY;
    const Ty iXZ = cylT + capT;
    return AQvec3<Ty>(iXZ, iY, iXZ);
}

// Arbitrary symmetric inertia tensor -> principal moments + the principal-axis
// rotation, folded into the body orientation PhysX/Chaos-style. Classic cyclic
// Jacobi eigendecomposition of a symmetric 3x3. `outAxis` rotates body-local
// vectors into the principal frame; `outMoments` are the eigenvalues.
template<class Ty>
inline void AQdiagonalizeInertia(const AQMat3<Ty>& I, AQVec3<Ty>& outMoments,
                                 OmegaGTE::Quaternion<Ty>& outAxis) {
    // Work on a mutable symmetric copy A; accumulate rotations into V.
    Ty a[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) a[i][j] = I[i][j];

    Ty v[3][3] = {{Ty(1),Ty(0),Ty(0)},{Ty(0),Ty(1),Ty(0)},{Ty(0),Ty(0),Ty(1)}};

    for (int sweep = 0; sweep < 32; ++sweep) {
        // Largest off-diagonal magnitude; stop when negligible.
        Ty off = std::abs(a[0][1]) + std::abs(a[0][2]) + std::abs(a[1][2]);
        if (off < Ty(1e-20)) break;

        for (int p = 0; p < 2; ++p) {
            for (int q = p + 1; q < 3; ++q) {
                if (std::abs(a[p][q]) < Ty(1e-20)) continue;
                const Ty theta = (a[q][q] - a[p][p]) / (Ty(2) * a[p][q]);
                const Ty tsign = (theta >= Ty(0)) ? Ty(1) : Ty(-1);
                const Ty tval  = tsign / (std::abs(theta) + std::sqrt(theta*theta + Ty(1)));
                const Ty c = Ty(1) / std::sqrt(tval*tval + Ty(1));
                const Ty s = tval * c;

                // Apply the Jacobi rotation A := Jᵀ A J on rows/cols p,q.
                for (int i = 0; i < 3; ++i) {
                    const Ty aip = a[i][p], aiq = a[i][q];
                    a[i][p] = c*aip - s*aiq;
                    a[i][q] = s*aip + c*aiq;
                }
                for (int i = 0; i < 3; ++i) {
                    const Ty api = a[p][i], aqi = a[q][i];
                    a[p][i] = c*api - s*aqi;
                    a[q][i] = s*api + c*aqi;
                }
                // Accumulate the eigenvectors.
                for (int i = 0; i < 3; ++i) {
                    const Ty vip = v[i][p], viq = v[i][q];
                    v[i][p] = c*vip - s*viq;
                    v[i][q] = s*vip + c*viq;
                }
            }
        }
    }

    outMoments = AQvec3<Ty>(a[0][0], a[1][1], a[2][2]);

    // V's COLUMNS are the principal-axis eigenvectors (math convention,
    // row-major: v[row][col]). Build the GTE 4×4 rotation matrix so its
    // standard-math entry at (row, col) is V(row, col) — i.e., the quaternion
    // we extract rotates +e_k to the k-th column of V. GTE stores entries as
    // `m[col][row]`, so m_std(row, col) == m_GTE[col][row]: writing
    // `m[i][j] = v[j][i]` puts m_std(row=j, col=i) == V(j, i). (The original
    // Phase 1 `m[i][j] = v[i][j]` accidentally stored Vᵀ — undetected because
    // the Phase 1 math test validates only eigenvalues, not the eigenvector
    // quaternion. Phase 1.1's full-tensor `addBody` is what exposed it.)
    auto m = OmegaGTE::Matrix<Ty,4,4>::Create();
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) m[i][j] = v[j][i];
    m[3][3] = Ty(1);
    outAxis = OmegaGTE::Quaternion<Ty>::fromMatrix(m).normalized();
}

// World-space inverse inertia:  R · diag(invMomentsBody) · Rᵀ.
// Built as Σ_k invMoments[k] · (aₖ aₖᵀ) where aₖ is the k-th body axis rotated
// into world by q — convention-safe (no dependence on the 4x4 matrix layout).
template<class Ty>
inline AQMat3<Ty> AQworldInvInertia(const OmegaGTE::Quaternion<Ty>& q,
                                    const AQVec3<Ty>& invMomentsBody) {
    const AQVec3<Ty> axis[3] = {
        AQrotate(q, AQvec3<Ty>(Ty(1), Ty(0), Ty(0))),
        AQrotate(q, AQvec3<Ty>(Ty(0), Ty(1), Ty(0))),
        AQrotate(q, AQvec3<Ty>(Ty(0), Ty(0), Ty(1))),
    };
    auto m = AQMat3<Ty>::Create();
    for (int k = 0; k < 3; ++k) {
        const Ty d = invMomentsBody[k][0];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                m[i][j] += d * axis[k][i][0] * axis[k][j][0];
    }
    return m;
}

// --- rigid transform (position + orientation) ---
template<class Ty>
struct AQTransform {
    AQVec3<Ty>                 p = AQVec3<Ty>::Create();              // translation
    OmegaGTE::Quaternion<Ty>   q = OmegaGTE::Quaternion<Ty>::Identity(); // rotation

    // Homogeneous matrix following GTE's own translationMatrix slot convention
    // (translation in the m[3][*] column), so it composes with GTE 4x4s the way
    // the rest of the suite expects. Point/vector transforms below go through
    // AQrotate and are convention-independent.
    OmegaGTE::Matrix<Ty,4,4> toMatrix() const {
        auto m = q.toMatrix();
        m[3][0] = p[0][0]; m[3][1] = p[1][0]; m[3][2] = p[2][0];
        return m;
    }

    AQTransform inverse() const {
        AQTransform r;
        r.q = q.conjugate();                 // unit ⇒ conjugate == inverse
        r.p = -AQrotate(r.q, p);
        return r;
    }

    // Compose: (this ∘ child) applies child first, then this.
    AQTransform operator*(const AQTransform& child) const {
        AQTransform r;
        r.q = q * child.q;
        r.p = p + AQrotate(q, child.p);
        return r;
    }

    AQVec3<Ty> transformPoint(const AQVec3<Ty>& v) const {
        return p + AQrotate(q, v);
    }
    AQVec3<Ty> transformVector(const AQVec3<Ty>& v) const {
        return AQrotate(q, v);
    }
};

// ====================================================================
// Axis-aligned bounding volume (Phase 1.1 §6.1; Phase 2 §7 drafted it,
// Phase 1.1 lands it here as foundational math so Phase 2's collision
// header consumes rather than redefines it). Empty-AABB convention is
// min=+inf, max=-inf so the first `merged()` is always correct.
// ====================================================================

template<class Ty>
struct AQAABB {
    // Default-initialize the GTE vectors via their Create() factory — the
    // Matrix default constructor is private (the same factory-only idiom
    // AQBodyState / AQDebugLine work around). A default-constructed AQAABB is
    // a zero-volume box at the origin; for accumulation use empty().
    AQVec3<Ty> min = AQVec3<Ty>::Create();
    AQVec3<Ty> max = AQVec3<Ty>::Create();

    static AQAABB empty() {
        const Ty hi = std::numeric_limits<Ty>::max();
        AQAABB b;
        b.min = AQvec3<Ty>( hi,  hi,  hi);
        b.max = AQvec3<Ty>(-hi, -hi, -hi);
        return b;
    }
    static AQAABB fromMinMax(const AQVec3<Ty>& lo, const AQVec3<Ty>& hi) {
        AQAABB b; b.min = lo; b.max = hi; return b;
    }
    static AQAABB fromCenterHalfExtents(const AQVec3<Ty>& c, const AQVec3<Ty>& h) {
        return fromMinMax(c - h, c + h);
    }

    bool overlaps(const AQAABB& o) const {
        return !(max[0][0] < o.min[0][0] || min[0][0] > o.max[0][0] ||
                 max[1][0] < o.min[1][0] || min[1][0] > o.max[1][0] ||
                 max[2][0] < o.min[2][0] || min[2][0] > o.max[2][0]);
    }
    AQAABB merged(const AQAABB& o) const {
        AQAABB r;
        r.min = AQvec3<Ty>(std::min(min[0][0], o.min[0][0]),
                           std::min(min[1][0], o.min[1][0]),
                           std::min(min[2][0], o.min[2][0]));
        r.max = AQvec3<Ty>(std::max(max[0][0], o.max[0][0]),
                           std::max(max[1][0], o.max[1][0]),
                           std::max(max[2][0], o.max[2][0]));
        return r;
    }
    AQAABB fattened(Ty margin) const {
        const auto m = AQvec3<Ty>(margin, margin, margin);
        AQAABB r; r.min = min - m; r.max = max + m; return r;
    }
    AQVec3<Ty> center() const {
        return (min + max) * Ty(0.5);
    }
    AQVec3<Ty> extents() const {                // full width along each axis
        return max - min;
    }
    Ty surfaceArea() const {
        const auto e = extents();
        const Ty x = e[0][0], y = e[1][0], z = e[2][0];
        return Ty(2) * (x * y + y * z + z * x);
    }
    bool contains(const AQVec3<Ty>& p) const {
        return p[0][0] >= min[0][0] && p[0][0] <= max[0][0] &&
               p[1][0] >= min[1][0] && p[1][0] <= max[1][0] &&
               p[2][0] >= min[2][0] && p[2][0] <= max[2][0];
    }
};
using FAABB = AQAABB<float>;

// |R|·h oriented-box world bound (Phase 2 §6.1). For a box with half-extents h
// rotated by q about `center`, the world-axis-aligned half-extent along axis i
// is Σ_j |R_ij| · h_j — the matrix of absolute values of the rotation times h.
// Cheap, correct under rotation, and always contains the box's 8 corners.
//
// Numerical hardening: the formula above is mathematically exact, but the
// path that computes a *single corner* (`center + R · ±h`) accumulates a
// different set of float multiply-add roundings than this per-axis sum, so
// in float a corner can land a couple of ULP outside the computed bound.
// The Phase 2 broadphase relies on the "always contains" invariant — a
// missed corner means a missed pair, the §2-point-2 catastrophe. Pad the
// half-extents by a small scale-aware ε absorbing up to ~8 ULP of float
// drift across either computation path; the pad is sub-micron at human
// scale and invisible to broadphase fattening, but it makes the
// contains-all-corners guarantee robust under float roundoff.
template<class Ty>
inline AQAABB<Ty> AQaabbOfOrientedBox(const AQVec3<Ty>& centerW,
                                      const AQVec3<Ty>& halfExtents,
                                      const OmegaGTE::Quaternion<Ty>& q) {
    const AQVec3<Ty> rx = AQrotate(q, AQvec3<Ty>(Ty(1), Ty(0), Ty(0)));
    const AQVec3<Ty> ry = AQrotate(q, AQvec3<Ty>(Ty(0), Ty(1), Ty(0)));
    const AQVec3<Ty> rz = AQrotate(q, AQvec3<Ty>(Ty(0), Ty(0), Ty(1)));
    const Ty hx = halfExtents[0][0], hy = halfExtents[1][0], hz = halfExtents[2][0];

    const Ty pad = Ty(8) * std::numeric_limits<Ty>::epsilon() *
                   (std::abs(centerW[0][0]) + std::abs(centerW[1][0]) + std::abs(centerW[2][0]) +
                    hx + hy + hz);

    const auto h = AQvec3<Ty>(
        std::abs(rx[0][0]) * hx + std::abs(ry[0][0]) * hy + std::abs(rz[0][0]) * hz + pad,
        std::abs(rx[1][0]) * hx + std::abs(ry[1][0]) * hy + std::abs(rz[1][0]) * hz + pad,
        std::abs(rx[2][0]) * hx + std::abs(ry[2][0]) * hy + std::abs(rz[2][0]) * hz + pad);
    return AQAABB<Ty>::fromCenterHalfExtents(centerW, h);
}

#endif // AQUA_AQMATH_H
