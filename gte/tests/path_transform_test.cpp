/// Backend-independent unit test for the GVectorPath transform helpers in
/// GTEBase.h (`OmegaGTE::translate` / `OmegaGTE::rotate`). Pure CPU — these are
/// header-only trig over a linked list of points, no device needed.
///
/// The regression this exists to prevent: the path rotations were written with
/// every sine term negated, so they applied the TRANSPOSE of the rotation they
/// documented — a path rotated by +90° turned -90°, the opposite way from
/// `rotationZ`, `rotationEuler`, `FQuaternion` and therefore from GESpace. The
/// order (pitch, then yaw, then roll) was already correct; only the handedness
/// was wrong.
///
/// The helpers are implemented with direct trig rather than FMatrix (GTEMath.h
/// depends on GTEBase.h, so the transforms cannot use it), which is exactly why
/// they can drift from the matrix builders without anything noticing. These
/// checks tie the two implementations together: every assertion below compares a
/// rotated path against the SAME rotation built as a matrix in GTEMath.h.

#include <omegaGTE/GTEBase.h>
#include <omegaGTE/GTEMath.h>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace OmegaGTE;

namespace {

constexpr float kEps = 1e-4f;

bool nearlyEqual(float a, float b, float eps = kEps) {
    return std::fabs(a - b) <= eps;
}

/// Collect a path's points in order.
std::vector<GPoint3D> pointsOf(GVectorPath3D &path) {
    std::vector<GPoint3D> out;
    path.transformEachPoint([&](GPoint3D &p) { out.push_back(p); });
    return out;
}

std::vector<GPoint2D> pointsOf(GVectorPath2D &path) {
    std::vector<GPoint2D> out;
    path.transformEachPoint([&](GPoint2D &p) { out.push_back(p); });
    return out;
}

void expect3D(const GPoint3D &got, const GPoint3D &want, const char *what) {
    if (!nearlyEqual(got.x, want.x) || !nearlyEqual(got.y, want.y) || !nearlyEqual(got.z, want.z)) {
        std::printf("FAIL %s: got (%f, %f, %f), want (%f, %f, %f)\n",
                    what, got.x, got.y, got.z, want.x, want.y, want.z);
        assert(false && "path transform mismatch");
    }
}

void expect2D(const GPoint2D &got, float x, float y, const char *what) {
    if (!nearlyEqual(got.x, x) || !nearlyEqual(got.y, y)) {
        std::printf("FAIL %s: got (%f, %f), want (%f, %f)\n", what, got.x, got.y, x, y);
        assert(false && "path transform mismatch");
    }
}

}  // namespace

int main() {
    const float quarter = HalfPi<float>;

    // --- 2D rotation matches rotationZ, in DIRECTION as well as magnitude. ---
    //     +90 deg about Z takes (1,0) to (0,1). The transposed version returned
    //     (0,-1): the right angle, the wrong way round.
    {
        GVectorPath2D path(GPoint2D{1.f, 0.f});
        path.append(GPoint2D{0.f, 1.f});
        rotate(path, quarter);

        const auto pts = pointsOf(path);
        assert(pts.size() == 2);
        expect2D(pts[0], 0.f, 1.f, "2D rotate(+90) takes (1,0) to (0,1)");
        expect2D(pts[1], -1.f, 0.f, "2D rotate(+90) takes (0,1) to (-1,0)");
    }

    // --- 2D rotation agrees with rotationZ() pointwise, at arbitrary angles. --
    {
        const float angles[] = {0.f, 0.3f, -1.2f, quarter, 2.5f};
        const GPoint2D probes[] = {{1.f, 0.f}, {0.f, 1.f}, {3.f, -4.f}, {-2.f, -5.f}};

        for (float a : angles) {
            const auto m = rotationZ(a);
            for (const auto &probe : probes) {
                GVectorPath2D path(probe);
                rotate(path, a);
                const auto got = pointsOf(path).front();

                // The same rotation, built as a matrix in GTEMath.h.
                const auto want = transformPoint(m, GPoint3D{probe.x, probe.y, 0.f});
                if (!nearlyEqual(got.x, want.x) || !nearlyEqual(got.y, want.y)) {
                    std::printf("FAIL 2D rotate(%f) on (%f,%f): path gave (%f,%f), "
                                "rotationZ gave (%f,%f)\n",
                                a, probe.x, probe.y, got.x, got.y, want.x, want.y);
                    assert(false && "2D path rotate must agree with rotationZ");
                }
            }
        }
    }

    // --- 3D rotation agrees with rotationEuler() pointwise. This is the check
    //     that ties the direct-trig path helper to the matrix builder; it fails
    //     on ANY sign or ordering divergence between the two. -----------------
    {
        struct Euler { float pitch, yaw, roll; };
        const Euler eulers[] = {
            {quarter, 0.f, 0.f},        // pure pitch
            {0.f, quarter, 0.f},        // pure yaw
            {0.f, 0.f, quarter},        // pure roll
            {quarter, quarter, 0.f},    // the pair that exposed the order bug
            {0.3f, -0.7f, 1.1f},        // arbitrary
            {-1.4f, 2.2f, -0.5f},
        };
        const GPoint3D probes[] = {
            {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}, {1.f, -2.f, 3.f},
        };

        for (const auto &e : eulers) {
            const auto m = rotationEuler(e.pitch, e.yaw, e.roll);
            const auto q = FQuaternion::fromEuler(e.pitch, e.yaw, e.roll).toMatrix();

            for (const auto &probe : probes) {
                GVectorPath3D path(probe);
                rotate(path, e.pitch, e.yaw, e.roll);
                const auto got = pointsOf(path).front();

                expect3D(got, transformPoint(m, probe), "3D path rotate vs rotationEuler");
                expect3D(got, transformPoint(q, probe), "3D path rotate vs FQuaternion::fromEuler");
            }
        }
    }

    // --- The docstring's explicit promise: a 2D path rotated by `radians` and
    //     the same path embedded in the XY-plane rotated by `roll = radians`
    //     agree. Both were wrong before, so both agreed — this now pins them to
    //     the CORRECT shared handedness rather than a shared mistake. ---------
    {
        const float a = 0.9f;
        const GPoint2D probe{2.f, -3.f};

        GVectorPath2D flat(probe);
        rotate(flat, a);
        const auto flatPt = pointsOf(flat).front();

        GVectorPath3D embedded(GPoint3D{probe.x, probe.y, 0.f});
        rotate(embedded, 0.f, 0.f, a);   // roll only
        const auto embeddedPt = pointsOf(embedded).front();

        expect3D(GPoint3D{flatPt.x, flatPt.y, 0.f}, embeddedPt,
                 "a 2D rotation and a roll-only 3D rotation must agree");
    }

    // --- Rotation about a pivot leaves the pivot fixed and rotates around it. -
    {
        const GPoint3D pivot{10.f, 10.f, 0.f};
        GVectorPath3D path(pivot);
        path.append(GPoint3D{11.f, 10.f, 0.f});   // one unit +X of the pivot

        rotate(path, 0.f, 0.f, quarter, pivot);

        const auto pts = pointsOf(path);
        expect3D(pts[0], pivot, "the pivot itself must not move");
        // +90 about Z sends the +X offset to +Y.
        expect3D(pts[1], GPoint3D{10.f, 11.f, 0.f}, "points rotate about the pivot");
    }

    // --- Translation, 2D and 3D. No sign subtlety here, but it is the one path
    //     helper with live callers (WTK's Path.cpp), so pin it. ---------------
    {
        GVectorPath2D p2(GPoint2D{1.f, 2.f});
        p2.append(GPoint2D{3.f, 4.f});
        translate(p2, 10.f, 20.f);
        const auto pts2 = pointsOf(p2);
        expect2D(pts2[0], 11.f, 22.f, "2D translate, first point");
        expect2D(pts2[1], 13.f, 24.f, "2D translate, second point");

        GVectorPath3D p3(GPoint3D{1.f, 2.f, 3.f});
        p3.append(GPoint3D{4.f, 5.f, 6.f});
        translate(p3, 10.f, 20.f, 30.f);
        const auto pts3 = pointsOf(p3);
        expect3D(pts3[0], GPoint3D{11.f, 22.f, 33.f}, "3D translate, first point");
        expect3D(pts3[1], GPoint3D{14.f, 25.f, 36.f}, "3D translate, second point");
    }

    // --- Every point of a multi-point path is transformed, not just the head. -
    {
        GVectorPath3D path(GPoint3D{1.f, 0.f, 0.f});
        path.append(GPoint3D{2.f, 0.f, 0.f});
        path.append(GPoint3D{3.f, 0.f, 0.f});
        rotate(path, 0.f, 0.f, quarter);

        const auto pts = pointsOf(path);
        assert(pts.size() == 3 && "transformEachPoint must visit every point");
        for (size_t i = 0; i < pts.size(); i++) {
            const float expected = static_cast<float>(i) + 1.f;
            expect3D(pts[i], GPoint3D{0.f, expected, 0.f}, "every point rotates");
        }
    }

    std::printf("path_transform_test: all checks passed\n");
    return 0;
}
