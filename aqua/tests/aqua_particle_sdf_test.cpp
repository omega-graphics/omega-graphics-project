// AQUA Phase 6 §13.3 6a — closed-form point-vs-shape signed-distance validation.
//
// AQshapeSignedDistance is the exact analytic collision query the particle
// push-out (Pass D) rides on. The proposal assumed Phase 2 shipped it; it did
// not, so it lands in Phase 6 and this test pins its closed forms against
// hand-computed distances and normal directions for plane / sphere / box /
// capsule, including a translated and a rotated body so the world-frame
// transform path is exercised. No framework — the standalone-main()/check()
// idiom the other aqua tests use.

#include <aqua/AQCollision.h>
#include <aqua/AQMath.h>
#include <aqua/AQParticles.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <type_traits>

using OmegaGTE::FVec;

// The §7 POD types must be trivially COPYABLE (so an SoA / parameter block blits
// to a GPU buffer with a plain memcpy) and standard-layout. The FVec members
// give them a non-trivial default ctor, so they are NOT trivially default-
// constructible — only copyable — which is exactly what the upload path needs.
static_assert(std::is_trivially_copyable<AQEmitter>::value,    "AQEmitter must be trivially copyable");
static_assert(std::is_standard_layout<AQEmitter>::value,       "AQEmitter must be standard-layout");
static_assert(std::is_trivially_copyable<AQForceField>::value, "AQForceField must be trivially copyable");
static_assert(std::is_standard_layout<AQForceField>::value,    "AQForceField must be standard-layout");
static_assert(std::is_trivially_copyable<AQParticlePool>::value, "AQParticlePool must be trivially copyable");
static_assert(std::is_trivially_copyable<AQShapeSample>::value,  "AQShapeSample must be trivially copyable");

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    std::printf("[%s] %s\n", cond ? "PASS" : "FAIL", what.c_str());
    if (!cond) ++g_failures;
}

static float vlen(const FVec<3>& v) { return std::sqrt(OmegaGTE::dot(v, v)); }

// distance close to expected
static bool dclose(float d, float expected, float tol = 1e-4f) {
    return std::fabs(d - expected) <= tol;
}

// normal close to a target unit direction (components within tol)
static bool nclose(const FVec<3>& n, float x, float y, float z, float tol = 1e-4f) {
    return std::fabs(n[0][0] - x) <= tol &&
           std::fabs(n[1][0] - y) <= tol &&
           std::fabs(n[2][0] - z) <= tol;
}

static AQShape makeSphere(float r) {
    AQShape s; s.type = AQShapeType::Sphere; s.sphere.radius = r; return s;
}
static AQShape makeBox(float hx, float hy, float hz) {
    AQShape s; s.type = AQShapeType::Box; s.box = {hx, hy, hz}; return s;
}
static AQShape makePlane(float nx, float ny, float nz, float offset) {
    AQShape s; s.type = AQShapeType::Plane; s.plane = {nx, ny, nz, offset}; return s;
}
static AQShape makeCapsule(float radius, float halfHeight) {
    AQShape s; s.type = AQShapeType::Capsule; s.capsule = {radius, halfHeight}; return s;
}

static void testPlane() {
    const AQShape plane = makePlane(0.f, 1.f, 0.f, 0.f);  // y = 0 half-space
    AQTransform<float> id;                                // identity body

    auto above = AQshapeSignedDistance(plane, AQvec3(0.f, 2.f, 0.f), id);
    check(dclose(above.distance, 2.f), "plane: point 2 above has distance +2");
    check(nclose(above.normal, 0.f, 1.f, 0.f), "plane: normal points +Y");

    auto below = AQshapeSignedDistance(plane, AQvec3(3.f, -1.f, -4.f), id);
    check(dclose(below.distance, -1.f), "plane: point 1 below has distance -1 (independent of x,z)");
    check(nclose(below.normal, 0.f, 1.f, 0.f), "plane: normal still +Y below");

    // Non-unit stored normal must still yield a true distance (offset scaled).
    const AQShape scaled = makePlane(0.f, 2.f, 0.f, 6.f);  // 2y = 6  ->  y = 3
    auto onScaled = AQshapeSignedDistance(scaled, AQvec3(0.f, 5.f, 0.f), id);
    check(dclose(onScaled.distance, 2.f), "plane: non-unit normal normalized (y=5 vs plane y=3 -> +2)");
    check(nclose(onScaled.normal, 0.f, 1.f, 0.f), "plane: non-unit normal normalized to +Y");
}

static void testSphere() {
    const AQShape sph = makeSphere(1.f);
    AQTransform<float> id;

    auto out = AQshapeSignedDistance(sph, AQvec3(3.f, 0.f, 0.f), id);
    check(dclose(out.distance, 2.f), "sphere r=1: point at x=3 has distance +2");
    check(nclose(out.normal, 1.f, 0.f, 0.f), "sphere: normal points +X");

    auto in = AQshapeSignedDistance(sph, AQvec3(0.f, 0.f, 0.f), id);
    check(dclose(in.distance, -1.f), "sphere: centre has distance -radius");
    check(dclose(vlen(in.normal), 1.f), "sphere: degenerate-centre normal is still unit length");

    // Translated body: sphere centre at (10,0,0), query at (10,0,4) -> d = 3.
    AQTransform<float> t; t.p = AQvec3(10.f, 0.f, 0.f);
    auto tr = AQshapeSignedDistance(sph, AQvec3(10.f, 0.f, 4.f), t);
    check(dclose(tr.distance, 3.f), "sphere: translated body, distance is world-correct");
    check(nclose(tr.normal, 0.f, 0.f, 1.f), "sphere: translated body, normal +Z");
}

static void testBox() {
    const AQShape box = makeBox(1.f, 1.f, 1.f);
    AQTransform<float> id;

    auto face = AQshapeSignedDistance(box, AQvec3(2.f, 0.f, 0.f), id);
    check(dclose(face.distance, 1.f), "box h=1: point at x=2 has distance +1 (face)");
    check(nclose(face.normal, 1.f, 0.f, 0.f), "box: face normal +X");

    // Exterior corner: q = (1,1,0) -> distance sqrt(2), normal (1/√2,1/√2,0).
    auto corner = AQshapeSignedDistance(box, AQvec3(2.f, 2.f, 0.f), id);
    check(dclose(corner.distance, std::sqrt(2.f)), "box: exterior corner distance = sqrt(2)");
    const float inv2 = 1.f / std::sqrt(2.f);
    check(nclose(corner.normal, inv2, inv2, 0.f), "box: corner normal is the diagonal");

    // Interior point: nearest face is +X (least penetration).
    auto inside = AQshapeSignedDistance(box, AQvec3(0.6f, 0.f, 0.f), id);
    check(dclose(inside.distance, -0.4f), "box: interior distance is negative to nearest face");
    check(nclose(inside.normal, 1.f, 0.f, 0.f), "box: interior exit normal is nearest face +X");
}

static void testCapsule() {
    const AQShape cap = makeCapsule(0.5f, 1.f);  // radius .5, half-height 1 on +Y
    AQTransform<float> id;

    // Beside the cylinder: nearest segment point is the origin, d = 2 - 0.5.
    auto side = AQshapeSignedDistance(cap, AQvec3(2.f, 0.f, 0.f), id);
    check(dclose(side.distance, 1.5f), "capsule: point beside cylinder, distance = |p|-r");
    check(nclose(side.normal, 1.f, 0.f, 0.f), "capsule: side normal +X");

    // Above the top cap: y clamps to +1, delta = (0,2,0), d = 2 - 0.5.
    auto top = AQshapeSignedDistance(cap, AQvec3(0.f, 3.f, 0.f), id);
    check(dclose(top.distance, 1.5f), "capsule: point above cap, segment clamps to hemisphere");
    check(nclose(top.normal, 0.f, 1.f, 0.f), "capsule: cap normal +Y");
}

static void testRotatedBody() {
    // Body rotated +90 deg about Z. Local +Y maps to world -X.
    AQTransform<float> t;
    t.q = AQquatExp(AQvec3(0.f, 0.f, 3.14159265358979323846f / 4.f));  // half-angle => 90 deg
    const AQShape plane = makePlane(0.f, 1.f, 0.f, 0.f);               // local y=0 half-space

    auto hit = AQshapeSignedDistance(plane, AQvec3(-2.f, 0.f, 0.f), t);
    check(dclose(hit.distance, 2.f, 1e-3f), "rotated plane: world point along rotated normal, distance +2");
    check(nclose(hit.normal, -1.f, 0.f, 0.f, 1e-3f), "rotated plane: world normal rotated to -X");
}

int main() {
    testPlane();
    testSphere();
    testBox();
    testCapsule();
    testRotatedBody();

    std::printf("\n%s: %d failure(s)\n", g_failures == 0 ? "OK" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
