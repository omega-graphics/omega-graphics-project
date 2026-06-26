// AQUA Phase 3 — GJK (Gilbert-Johnson-Keerthi 1988) + EPA (van den Bergen 2001)
// general convex/convex contact. Consumes AQshapeSupport (Phase 2 §7) — no
// new shape vocabulary. Used by the AQNarrowphase.cpp dispatcher for every
// pair where both shapes are bounded convex and there is no specialized
// closed-form path (box/capsule, anything with a hull and a bounded
// partner). Plane half-spaces never reach here — they have no bounded
// support and route to the specialized plane paths in AQNarrowphase.cpp.
//
// Implementation notes (Phase-3 brief §4, Phase-3 §12 recency addendum):
//   * Distance phase (GJK): iterative-closest-point on the Minkowski-
//     difference simplex (Christer Ericson §9.5 / van den Bergen Algorithm
//     5). Each iteration adds the support point in the direction of origin
//     and reduces the simplex to the closest sub-feature. The iteration
//     kernel uses the Nesterov-accelerated variant (Montaut, Le Lidec,
//     Petrik, Sivic, Carpentier, "Collision Detection Accelerated: An
//     Optimization Perspective," RSS 2024 — the Phase 3.x recency-audit
//     adopt-now finding, mirrored in `aqua/.plans/
//     Phase-3-Narrowphase-Contact-Solver.md` §12): the search direction
//     carries a momentum term that empirically halves iteration counts on
//     typical pairs and gives larger wins at the worst-case cliff GJK has
//     historically lived on. Correctness is preserved by the Frank-Wolfe
//     duality-gap fallback — when the accelerated direction stops making
//     progress toward the optimum, the loop reverts to classical GJK for
//     the remaining iterations. Same support-function interface
//     (`AQshapeSupport`), same termination contract (a separating
//     direction proves no contact; an origin-enclosing tetrahedron hands
//     EPA a seed); the EPA polytope expansion below is unchanged.
//   * Penetration phase (EPA): expand polytope by adding the support in the
//     current closest face's outward normal until the support direction stops
//     producing new vertices (van den Bergen 2001). The polytope is
//     represented as triangle faces with explicit vertices/edges; the
//     silhouette re-triangulation removes all faces visible from the new
//     vertex and stitches new triangles to the silhouette boundary.
//   * Witness recovery: the closest face's barycentric weights on the
//     Minkowski-difference points map back to the original support points on
//     A and B (we keep those alongside the difference point), so the contact
//     point is the midpoint of the two witness points.
//
// Returns a single contact point per pair; for box/box and other multi-point
// resting contacts the dispatcher uses a specialized path instead (the
// settling-stack deliverable in §1 lives entirely on those — GJK/EPA is the
// general fallback that keeps Phase 3 complete for arbitrary convex shapes).

#include <aqua/AQCollision.h>
#include <aqua/AQContact.h>
#include <aqua/AQMath.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

using OmegaGTE::FVec;

namespace {

// Minkowski-difference support point — keeps the contributing support points
// on A and B alongside the difference so EPA witness recovery can map face
// barycentrics back to a contact point pair without re-running support.
struct SupportPt {
    FVec<3> p = AQvec3(0.f, 0.f, 0.f);    // supportA(d) - supportB(-d)
    FVec<3> sa = AQvec3(0.f, 0.f, 0.f);   // supportA(d)
    FVec<3> sb = AQvec3(0.f, 0.f, 0.f);   // supportB(-d)
};

inline SupportPt mdSupport(const AQShape &A, const AQShape &B,
                           const AQTransform<float> &xfA, const AQTransform<float> &xfB,
                           const FVec<3> *hullVerts, std::size_t hullVertCount,
                           const FVec<3> &dir) {
    SupportPt sp;
    sp.sa = AQshapeSupport(A, dir,          xfA, hullVerts, hullVertCount);
    sp.sb = AQshapeSupport(B, dir * -1.f,   xfB, hullVerts, hullVertCount);
    sp.p  = sp.sa - sp.sb;
    return sp;
}

// Closest point on triangle (a, b, c) to the origin in 3D. Returns the
// barycentric weights (wa, wb, wc) such that closest = wa·a + wb·b + wc·c
// and wa + wb + wc = 1. Standard Real-Time Collision Detection §5.1.5
// formulation. The output triangle test (`inside`) is true when the
// origin's perpendicular projection onto the triangle's plane lies inside
// the triangle — the rest of the cases hand back an edge or vertex.
bool closestOnTriangle(const FVec<3> &a, const FVec<3> &b, const FVec<3> &c,
                       float &wa, float &wb, float &wc) {
    const FVec<3> ab = b - a;
    const FVec<3> ac = c - a;
    const FVec<3> ap = a * -1.f;                    // origin - a
    const float d1 = OmegaGTE::dot(ab, ap);
    const float d2 = OmegaGTE::dot(ac, ap);
    if (d1 <= 0.f && d2 <= 0.f) { wa = 1.f; wb = 0.f; wc = 0.f; return false; }
    const FVec<3> bp = b * -1.f;
    const float d3 = OmegaGTE::dot(ab, bp);
    const float d4 = OmegaGTE::dot(ac, bp);
    if (d3 >= 0.f && d4 <= d3) { wa = 0.f; wb = 1.f; wc = 0.f; return false; }
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.f && d1 >= 0.f && d3 <= 0.f) {
        const float v = d1 / (d1 - d3);
        wa = 1.f - v; wb = v; wc = 0.f; return false;
    }
    const FVec<3> cp = c * -1.f;
    const float d5 = OmegaGTE::dot(ab, cp);
    const float d6 = OmegaGTE::dot(ac, cp);
    if (d6 >= 0.f && d5 <= d6) { wa = 0.f; wb = 0.f; wc = 1.f; return false; }
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.f && d2 >= 0.f && d6 <= 0.f) {
        const float w = d2 / (d2 - d6);
        wa = 1.f - w; wb = 0.f; wc = w; return false;
    }
    const float va = d3 * d6 - d5 * d4;
    const float denom1 = (d4 - d3) + (d5 - d6);
    if (va <= 0.f && (d4 - d3) >= 0.f && (d5 - d6) >= 0.f && denom1 > 0.f) {
        const float w = (d4 - d3) / denom1;
        wa = 0.f; wb = 1.f - w; wc = w; return false;
    }
    const float denom = 1.f / (va + vb + vc);
    wb = vb * denom; wc = vc * denom; wa = 1.f - wb - wc;
    return true;
}

// Closest point on a segment a-b to origin. Returns the parameter `t` such
// that closest = a + t·(b - a) and the closest point. Clamps to [0, 1].
FVec<3> closestOnSegment(const FVec<3> &a, const FVec<3> &b, float &t) {
    const FVec<3> ab = b - a;
    const float denom = OmegaGTE::dot(ab, ab);
    t = (denom > 1e-12f) ? std::clamp(-OmegaGTE::dot(a, ab) / denom, 0.f, 1.f) : 0.f;
    return a + ab * t;
}

// Compute closest point on the current simplex to the origin. Reduces the
// simplex (shifts `s[]` in place, updates `n`) so it contains only the
// vertices that contribute to the closest point. Returns true iff origin is
// strictly inside the simplex (only possible for n == 4).
bool simplexClosest(SupportPt *s, int &n, FVec<3> &closest) {
    if (n == 1) {
        closest = s[0].p;
        return false;
    }
    if (n == 2) {
        float t = 0.f;
        closest = closestOnSegment(s[0].p, s[1].p, t);
        if (t <= 0.f)      { n = 1; }
        else if (t >= 1.f) { n = 1; s[0] = s[1]; }
        return false;
    }
    if (n == 3) {
        float wa, wb, wc;
        closestOnTriangle(s[0].p, s[1].p, s[2].p, wa, wb, wc);
        closest = s[0].p * wa + s[1].p * wb + s[2].p * wc;
        SupportPt tmp[3]; int kept = 0;
        if (wa > 0.f) tmp[kept++] = s[0];
        if (wb > 0.f) tmp[kept++] = s[1];
        if (wc > 0.f) tmp[kept++] = s[2];
        if (kept == 0) { kept = 1; tmp[0] = s[0]; }   // guard against all-zero edge case
        n = kept;
        for (int i = 0; i < kept; ++i) s[i] = tmp[i];
        return false;
    }
    if (n == 4) {
        // 4 faces of the tetrahedron. For each face opposing vertex `opp`,
        // orient the face normal away from `opp`. Origin is outside the
        // tetrahedron iff it is on the outward side of at least one face;
        // we keep the closest such face and reduce the simplex to its
        // surviving vertices. Origin enclosed → collision (return true).
        bool outsideAny = false;
        int bestFi = -1;
        float bestDist2 = std::numeric_limits<float>::max();
        SupportPt bestKept[3];
        int bestKeptN = 0;
        FVec<3> bestClosest = AQvec3(0.f, 0.f, 0.f);
        for (int opp = 0; opp < 4; ++opp) {
            int idx[3]; int k = 0;
            for (int j = 0; j < 4; ++j) if (j != opp) idx[k++] = j;
            const FVec<3> a = s[idx[0]].p, b = s[idx[1]].p, c = s[idx[2]].p, opv = s[opp].p;
            FVec<3> normal = OmegaGTE::cross(b - a, c - a);
            if (OmegaGTE::dot(normal, opv - a) > 0.f) normal = normal * -1.f;
            const float side = OmegaGTE::dot(normal, a * -1.f);
            if (side < 0.f) continue;
            outsideAny = true;
            float wa, wb, wc;
            closestOnTriangle(a, b, c, wa, wb, wc);
            const FVec<3> p = a * wa + b * wb + c * wc;
            const float d2 = OmegaGTE::dot(p, p);
            if (d2 < bestDist2) {
                bestDist2 = d2;
                bestFi = opp;
                int kept = 0;
                if (wa > 0.f) bestKept[kept++] = s[idx[0]];
                if (wb > 0.f) bestKept[kept++] = s[idx[1]];
                if (wc > 0.f) bestKept[kept++] = s[idx[2]];
                if (kept == 0) { kept = 1; bestKept[0] = s[idx[0]]; }
                bestKeptN = kept;
                bestClosest = p;
            }
        }
        if (!outsideAny) return true;       // origin enclosed
        (void)bestFi;
        n = bestKeptN;
        for (int i = 0; i < bestKeptN; ++i) s[i] = bestKept[i];
        closest = bestClosest;
        return false;
    }
    closest = AQvec3(0.f, 0.f, 0.f);
    return false;
}

// GJK distance phase. Iterates until either origin is enclosed (returns
// true; simplex left as a tetrahedron for EPA) or the support direction
// stops finding new vertices that pass origin (returns false; the bodies
// are separated). Bounded at 64 iterations as a structural safety cap —
// converges within a handful of iterations for normal inputs.
//
// Iteration kernel uses Nesterov-accelerated GJK (Montaut et al. RSS 2024).
// Classical GJK is exactly Frank-Wolfe over the Minkowski-difference
// constraint set minimising ‖x‖²; the gradient at the current closest
// point `s_k` is `2·s_k`, so the support call `mdSupport(-s_k)` is the
// FW linear-minimisation oracle. The acceleration carries a momentum
// vector across iterations:
//   μ_k = (k + 1) / (k + 3)                     // standard Nesterov decay
//   y_k = μ_k · s_k + (1 − μ_k) · w_{k−1}        // lookahead blend
//   d_k = μ_k · d_{k−1} + (1 − μ_k) · (−y_k)     // momentum-blended search dir
// and recovers correctness via the Frank-Wolfe duality gap
//   g_k = 2 · ⟨s_{k−1}, s_{k−1} − w_k⟩
// computed against the previous-iteration closest point. When g_k drops
// below the gap tolerance the accelerated direction is no longer making
// progress toward the optimum; the loop reverts to classical GJK
// (d = −s) for the remainder. The simplex closest-point reduction
// (`simplexClosest`) is unchanged from classical GJK; the separating-
// direction termination test (`dot(w, dir) < 0`) is unchanged; the
// origin-enclosing case still hands EPA the tetrahedron it expects.
bool gjk(const AQShape &A, const AQShape &B,
         const AQTransform<float> &xfA, const AQTransform<float> &xfB,
         const FVec<3> *hullVerts, std::size_t hullVertCount,
         SupportPt simplex[4], int &simplexN) {
    const FVec<3> cA = (xfA * AQTransform<float>{AQvec3(A.lpx, A.lpy, A.lpz),
                                                  {A.lqx, A.lqy, A.lqz, A.lqw}}).p;
    const FVec<3> cB = (xfB * AQTransform<float>{AQvec3(B.lpx, B.lpy, B.lpz),
                                                  {B.lqx, B.lqy, B.lqz, B.lqw}}).p;
    FVec<3> dir = cB - cA;
    if (OmegaGTE::dot(dir, dir) < 1e-12f) dir = AQvec3(1.f, 0.f, 0.f);

    simplex[0] = mdSupport(A, B, xfA, xfB, hullVerts, hullVertCount, dir);
    simplexN = 1;
    dir = simplex[0].p * -1.f;
    if (OmegaGTE::dot(dir, dir) < 1e-12f) {
        // Origin already on the first support — collision (degenerate origin
        // case). Build a non-degenerate tetrahedron via three more supports
        // along orthogonal axes so EPA has something to work with.
        simplex[1] = mdSupport(A, B, xfA, xfB, hullVerts, hullVertCount, AQvec3(1.f, 0.f, 0.f));
        simplex[2] = mdSupport(A, B, xfA, xfB, hullVerts, hullVertCount, AQvec3(0.f, 1.f, 0.f));
        simplex[3] = mdSupport(A, B, xfA, xfB, hullVerts, hullVertCount, AQvec3(0.f, 0.f, 1.f));
        simplexN = 4;
        return true;
    }

    // Nesterov-accelerated GJK state. `prevDir` is d_{k−1} (last
    // iteration's accelerated search direction). `prevClosest` is s_{k−1}
    // (last iteration's closest point on the simplex). Both initialise to
    // the iter-0 single-vertex case where the closest point IS the support
    // and the direction is the negated support — i.e. iter 0 with μ_0 = 1/3
    // collapses to classical GJK, and acceleration kicks in from iter 1
    // once an actual support history exists. `useNesterov` is the
    // reversion flag flipped by the FW-gap fallback.
    constexpr float kFWGapTolerance = 1e-6f;
    FVec<3> prevDir     = dir;
    FVec<3> prevClosest = simplex[0].p;
    bool    useNesterov = true;

    for (int iter = 0; iter < 64; ++iter) {
        if (simplexN >= 4) break;
        SupportPt sp = mdSupport(A, B, xfA, xfB, hullVerts, hullVertCount, dir);

        // Frank-Wolfe duality-gap fallback (Nesterov-GJK §3.2 in Montaut
        // 2024). Skip on iter 0 — there's no s_{k−1} to measure against
        // yet, and the iter-0 direction is the classical one by
        // construction (see prevDir initialisation above). The check
        // is on the OLD closest vs the NEW support; if accelerated `dir`
        // didn't move us further from origin, abandon the momentum and
        // retry this iteration with the classical direction. The
        // discarded `sp` is intentional — using a stale support here
        // would corrupt the simplex.
        if (useNesterov && iter > 0) {
            const float fwGap = 2.f * OmegaGTE::dot(prevClosest, prevClosest - sp.p);
            if (fwGap <= kFWGapTolerance) {
                useNesterov = false;
                dir         = prevClosest * -1.f;
                prevDir     = dir;
                continue;
            }
        }

        if (OmegaGTE::dot(sp.p, dir) < 0.f) return false;     // can't cross origin
        simplex[simplexN++] = sp;
        FVec<3> closest = AQvec3(0.f, 0.f, 0.f);
        const bool enclosed = simplexClosest(simplex, simplexN, closest);
        if (enclosed) return true;
        if (OmegaGTE::dot(closest, closest) < 1e-12f) {
            // Origin is on the simplex (numerically zero distance) — collision.
            // Promote whatever we have to a tetrahedron so EPA can start.
            while (simplexN < 4) {
                FVec<3> seed = AQvec3(1.f, 0.f, 0.f);
                if (simplexN >= 2) seed = OmegaGTE::cross(simplex[1].p - simplex[0].p, AQvec3(0.f, 1.f, 0.f));
                if (OmegaGTE::dot(seed, seed) < 1e-12f) seed = AQvec3(0.f, 1.f, 0.f);
                simplex[simplexN++] = mdSupport(A, B, xfA, xfB, hullVerts, hullVertCount, seed);
            }
            return true;
        }

        // Compute next iteration's search direction. Classical mode after
        // a fallback: dir = −closest. Nesterov mode: blend the previous
        // direction with a lookahead point that mixes the new closest and
        // the new support; the standard (k+1)/(k+3) coefficient schedule
        // damps the momentum across the iteration count.
        const FVec<3> classicalDir = closest * -1.f;
        if (useNesterov) {
            const float    mu = static_cast<float>(iter + 1) / static_cast<float>(iter + 3);
            const FVec<3>  y  = closest * mu + sp.p * (1.f - mu);
            dir = prevDir * mu + (y * -1.f) * (1.f - mu);
        } else {
            dir = classicalDir;
        }
        prevDir     = dir;
        prevClosest = closest;
    }
    return false;
}

// EPA polytope face — a triangle with explicit vertex indices, outward
// normal, and distance from origin. The face list grows as we expand the
// polytope toward the penetration extreme.
struct Face {
    int v[3] = {0, 0, 0};            // indices into `verts`
    FVec<3> normal = AQvec3(0.f, 1.f, 0.f);   // unit outward normal
    float   distance = 0.f;          // dot(normal, verts[v[0]].p) ≥ 0
};

// Compute face data (normal pointing outward = away from polytope centroid;
// distance from origin = dot(normal, vertex)). Caller supplies a reference
// "inside" point so we can orient the normal robustly even when the polytope
// has not yet been centered on origin.
void buildFace(Face &f, const OmegaCommon::Vector<SupportPt> &verts, const FVec<3> &interior) {
    const FVec<3> &a = verts[f.v[0]].p;
    const FVec<3> &b = verts[f.v[1]].p;
    const FVec<3> &c = verts[f.v[2]].p;
    FVec<3> n = OmegaGTE::cross(b - a, c - a);
    const float n2 = OmegaGTE::dot(n, n);
    if (n2 > 1e-20f) n = n * (1.f / std::sqrt(n2));
    else             n = AQvec3(0.f, 1.f, 0.f);
    // Outward: should point away from `interior`.
    if (OmegaGTE::dot(n, a - interior) < 0.f) {
        n = n * -1.f;
        std::swap(f.v[1], f.v[2]);   // keep winding consistent with the outward normal
    }
    f.normal = n;
    f.distance = OmegaGTE::dot(n, a);
}

} // namespace

// ----------------------------------------------------------------------------
// AQgjkEpaContact — the public-to-AQNarrowphase entry point.
// ----------------------------------------------------------------------------

bool AQgjkEpaContact(const AQShape &shapeA, const AQShape &shapeB,
                     const AQTransform<float> &xfA, const AQTransform<float> &xfB,
                     const FVec<3> *hullVerts, std::size_t hullVertCount,
                     AQContactManifold &out) {
    SupportPt simplex[4] = { SupportPt{}, SupportPt{}, SupportPt{}, SupportPt{} };
    int simplexN = 0;
    if (!gjk(shapeA, shapeB, xfA, xfB, hullVerts, hullVertCount, simplex, simplexN)) {
        return false;
    }
    if (simplexN < 4) return false;   // degenerate; treat as no contact

    // Reference interior point for face orientation: centroid of the four
    // simplex vertices. It's inside the Minkowski difference polytope by
    // construction (GJK produced an origin-enclosing tetrahedron).
    const FVec<3> interior = (simplex[0].p + simplex[1].p + simplex[2].p + simplex[3].p) * 0.25f;

    OmegaCommon::Vector<SupportPt> verts;
    verts.reserve(64);
    verts.push_back(simplex[0]);
    verts.push_back(simplex[1]);
    verts.push_back(simplex[2]);
    verts.push_back(simplex[3]);

    OmegaCommon::Vector<Face> faces;
    faces.reserve(64);
    {
        Face f;
        f.v[0] = 0; f.v[1] = 1; f.v[2] = 2; buildFace(f, verts, interior); faces.push_back(f);
        f.v[0] = 0; f.v[1] = 1; f.v[2] = 3; buildFace(f, verts, interior); faces.push_back(f);
        f.v[0] = 0; f.v[1] = 2; f.v[2] = 3; buildFace(f, verts, interior); faces.push_back(f);
        f.v[0] = 1; f.v[1] = 2; f.v[2] = 3; buildFace(f, verts, interior); faces.push_back(f);
    }

    constexpr int   kMaxIters = 64;
    constexpr float kTolerance = 1e-4f;
    int closestFace = 0;
    for (int iter = 0; iter < kMaxIters; ++iter) {
        // Pick face nearest to origin.
        closestFace = 0;
        float bestDist = faces[0].distance;
        for (std::size_t i = 1; i < faces.size(); ++i) {
            if (faces[i].distance < bestDist) {
                bestDist = faces[i].distance;
                closestFace = static_cast<int>(i);
            }
        }
        // Support in face normal direction; if it lies on the face plane
        // (within tolerance), we've found the penetration extreme.
        const FVec<3> dir = faces[closestFace].normal;
        const SupportPt sp = mdSupport(shapeA, shapeB, xfA, xfB,
                                       hullVerts, hullVertCount, dir);
        const float supDist = OmegaGTE::dot(sp.p, dir);
        if (supDist - bestDist < kTolerance) break;

        // Collect the silhouette: edges of faces whose outward normal sees
        // the new vertex. Remove those faces, then re-triangulate the
        // silhouette by connecting each edge to the new vertex.
        const int newIdx = static_cast<int>(verts.size());
        verts.push_back(sp);

        struct Edge { int a, b; };
        OmegaCommon::Vector<Edge> silhouette;
        silhouette.reserve(16);

        auto addEdge = [&silhouette](int a, int b) {
            for (std::size_t i = 0; i < silhouette.size(); ++i) {
                if (silhouette[i].a == b && silhouette[i].b == a) {
                    silhouette[i] = silhouette.back();
                    silhouette.pop_back();
                    return;
                }
            }
            silhouette.push_back({a, b});
        };

        for (std::size_t i = 0; i < faces.size();) {
            const Face &f = faces[i];
            if (OmegaGTE::dot(f.normal, sp.p - verts[f.v[0]].p) > 0.f) {
                addEdge(f.v[0], f.v[1]);
                addEdge(f.v[1], f.v[2]);
                addEdge(f.v[2], f.v[0]);
                faces[i] = faces.back();
                faces.pop_back();
            } else {
                ++i;
            }
        }
        if (silhouette.empty()) break;   // numerical degenerate — accept current best face

        for (std::size_t i = 0; i < silhouette.size(); ++i) {
            Face nf;
            nf.v[0] = silhouette[i].a;
            nf.v[1] = silhouette[i].b;
            nf.v[2] = newIdx;
            buildFace(nf, verts, interior);
            faces.push_back(nf);
        }
    }

    // Penetration normal/depth from the closest face.
    const Face &f = faces[closestFace];
    const FVec<3> nW    = f.normal;
    const float   depth = f.distance;

    // Witness recovery: barycentric weights on the closest face's
    // Minkowski-difference triangle, applied to the original support points
    // on A and B. Witness point on A = w_a·sa[v0] + w_b·sa[v1] + w_c·sa[v2].
    float wa, wb, wc;
    closestOnTriangle(verts[f.v[0]].p, verts[f.v[1]].p, verts[f.v[2]].p, wa, wb, wc);
    const FVec<3> witA = verts[f.v[0]].sa * wa + verts[f.v[1]].sa * wb + verts[f.v[2]].sa * wc;
    const FVec<3> witB = verts[f.v[0]].sb * wa + verts[f.v[1]].sb * wb + verts[f.v[2]].sb * wc;

    // Manifold normal "from A to B" — the closest face's outward normal
    // points from the polytope (which contains the origin) outward. In
    // Minkowski-difference convention that direction is `B - A`, i.e., from
    // A toward B. Match the rest of the narrowphase by adopting it directly.
    out.normalWorld = nW;
    out.pointCount  = 1;
    out.points[0].positionWorld = (witA + witB) * 0.5f;
    out.points[0].depth         = std::max(0.f, depth);
    out.points[0].featureKey    = 0;
    return true;
}
