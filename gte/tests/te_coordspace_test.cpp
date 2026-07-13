/// Triangulation-Engine-Completion-Plan Phase 9 — backend-independent unit test
/// for TE's coordinate space: the params-level viewport (9.1), its resolution
/// precedence (9.2), viewport-origin awareness (9.3), and local-space
/// triangulation (9.6). Pure CPU — `OmegaTriangulationEngineContext` is abstract
/// but device-independent, so a test subclass supplying the two pure virtuals
/// gives a real end-to-end `triangulateSync` with no GPU (mirrors
/// te_index_buffer_test.cpp).
///
/// The regression this suite exists to prevent is the one that malformed every
/// 3D primitive: `translateCoordsDefaultImpl` used to divide z<0 by
/// `viewport->nearDepth`, which is 0 for every viewport the engine builds — so
/// a sphere centered on z=0 came back with half its vertices at -infinity and a
/// discontinuity across the seam.

#include <omegaGTE/TE.h>
#include <omegaGTE/GTEMath.h>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <future>
#include <limits>

using namespace OmegaGTE;

namespace {

constexpr float kEps = 1e-4f;

bool nearlyEqual(float a, float b, float eps = kEps) {
    return std::fabs(a - b) <= eps;
}

bool pointNearlyEqual(const GPoint3D &a, const GPoint3D &b, float eps = kEps) {
    return nearlyEqual(a.x, b.x, eps) && nearlyEqual(a.y, b.y, eps) && nearlyEqual(a.z, b.z, eps);
}

/// A CPU-only TE context anchored to a viewport the test chooses. Supplies the
/// two pure virtuals (`translateCoords`, `triangulateOnGPU`) and nothing else —
/// every coordinate decision under test lives in the base class.
class TestTEContext : public OmegaTriangulationEngineContext {
public:
    GEViewport effective;

    explicit TestTEContext(const GEViewport &vp) : effective(vp) {}

    GEViewport getEffectiveViewport() override { return effective; }

    void translateCoords(float x, float y, float z, GEViewport *viewport,
                         float *xr, float *yr, float *zr) override {
        // The same forwarding every shipped backend does.
        if (viewport) {
            translateCoordsDefaultImpl(x, y, z, viewport, xr, yr, zr);
            return;
        }
        GEViewport vp = getEffectiveViewport();
        translateCoordsDefaultImpl(x, y, z, &vp, xr, yr, zr);
    }

    std::future<TETriangulationResult> triangulateOnGPU(
        const TETriangulationParams &params,
        GTEPolygonFrontFaceRotation frontFaceRotation,
        GEViewport *viewport) override {
        std::promise<TETriangulationResult> p;
        p.set_value(triangulateSync(params, frontFaceRotation, viewport));
        return p.get_future();
    }

    /// Exposes the protected default impl so the mapping can be asserted
    /// pointwise, not only through a whole primitive.
    void mapPoint(float x, float y, float z, GEViewport &vp,
                  float *xr, float *yr, float *zr) {
        translateCoordsDefaultImpl(x, y, z, &vp, xr, yr, zr);
    }
};

/// Walk every vertex of a result. Returns false if any coordinate is non-finite.
bool allFinite(const TETriangulationResult &r) {
    for (const auto &poly : r.mesh.vertexPolygons) {
        const GPoint3D pts[3] = {poly.a.pt, poly.b.pt, poly.c.pt};
        for (const auto &p : pts) {
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return false;
        }
    }
    return true;
}

struct Bounds {
    float minX = std::numeric_limits<float>::max(), maxX = std::numeric_limits<float>::lowest();
    float minY = std::numeric_limits<float>::max(), maxY = std::numeric_limits<float>::lowest();
    float minZ = std::numeric_limits<float>::max(), maxZ = std::numeric_limits<float>::lowest();
};

Bounds boundsOf(const TETriangulationResult &r) {
    Bounds b;
    for (const auto &poly : r.mesh.vertexPolygons) {
        const GPoint3D pts[3] = {poly.a.pt, poly.b.pt, poly.c.pt};
        for (const auto &p : pts) {
            b.minX = std::min(b.minX, p.x); b.maxX = std::max(b.maxX, p.x);
            b.minY = std::min(b.minY, p.y); b.maxY = std::max(b.maxY, p.y);
            b.minZ = std::min(b.minZ, p.z); b.maxZ = std::max(b.maxZ, p.z);
        }
    }
    return b;
}

}  // namespace

int main() {
    // Every viewport this engine builds has nearDepth = 0 — that is what made
    // the old z<0 branch a division by zero.
    const GEViewport screen{0.f, 0.f, 800.f, 600.f, 0.f, 1.f};

    // --- The regression: a sphere centered on z=0 straddles the seam that used
    //     to send half its vertices to -infinity. --------------------------
    {
        TestTEContext ctx(screen);
        GSphere sphere;
        sphere.center = GPoint3D{400.f, 300.f, 0.f};
        sphere.radius = 100.f;

        auto params = TETriangulationParams::Sphere(sphere);
        params.addAttachment(TETriangulationParams::Attachment::makeColor(makeColor(1.f, 1.f, 1.f, 1.f)));

        auto result = ctx.triangulateSync(params);
        assert(!result.mesh.vertexPolygons.empty() && "sphere produced no geometry");
        assert(allFinite(result) && "sphere has non-finite vertices (the nearDepth divide-by-zero)");

        // Depth is one continuous affine map now, so the sphere's z extent must
        // be an interval, not a torn pair of clusters. With near=0/far=1 the map
        // is z_ndc = z, so a radius-100 sphere spans [-100, +100] in z.
        const Bounds b = boundsOf(result);
        assert(nearlyEqual(b.minZ, -100.f, 1.f) && "sphere min z should map continuously");
        assert(nearlyEqual(b.maxZ, 100.f, 1.f) && "sphere max z should map continuously");
    }

    // --- The depth map itself: near -> 0, far -> 1, linear, no branches. ----
    {
        TestTEContext ctx(screen);
        GEViewport vp{0.f, 0.f, 800.f, 600.f, 10.f, 110.f};
        float x = 0.f, y = 0.f, z = 0.f;

        ctx.mapPoint(400.f, 300.f, 10.f, vp, &x, &y, &z);
        assert(nearlyEqual(z, 0.f) && "near plane must map to 0");
        ctx.mapPoint(400.f, 300.f, 110.f, vp, &x, &y, &z);
        assert(nearlyEqual(z, 1.f) && "far plane must map to 1");
        ctx.mapPoint(400.f, 300.f, 60.f, vp, &x, &y, &z);
        assert(nearlyEqual(z, 0.5f) && "mid depth must map to 0.5");

        // Negative z is now finite and continuous with the rest of the map —
        // this is the exact input that used to divide by nearDepth = 0.
        GEViewport zeroNear{0.f, 0.f, 800.f, 600.f, 0.f, 1.f};
        ctx.mapPoint(400.f, 300.f, -50.f, zeroNear, &x, &y, &z);
        assert(std::isfinite(z) && "negative z must be finite");
        assert(nearlyEqual(z, -50.f) && "negative z must stay on the same affine line");
    }

    // --- 9.3: the viewport origin is honored. -------------------------------
    {
        TestTEContext ctx(screen);
        GEViewport offset{100.f, 50.f, 400.f, 200.f, 0.f, 1.f};
        float x = 0.f, y = 0.f, z = 0.f;

        // An offset viewport maps its OWN top-left to NDC (-1, +1).
        ctx.mapPoint(100.f, 50.f, 0.f, offset, &x, &y, &z);
        assert(nearlyEqual(x, -1.f) && nearlyEqual(y, 1.f) && "offset viewport top-left -> (-1,+1)");
        ctx.mapPoint(500.f, 250.f, 0.f, offset, &x, &y, &z);
        assert(nearlyEqual(x, 1.f) && nearlyEqual(y, -1.f) && "offset viewport bottom-right -> (+1,-1)");
        ctx.mapPoint(300.f, 150.f, 0.f, offset, &x, &y, &z);
        assert(nearlyEqual(x, 0.f) && nearlyEqual(y, 0.f) && "offset viewport center -> origin");
    }

    // --- 9.1 + 9.2: the params viewport is used, and it beats the call arg. --
    {
        TestTEContext ctx(screen);
        GRect rect;
        rect.pos = GPoint2D{0.f, 0.f};
        rect.w = 200.f;
        rect.h = 100.f;

        // (a) An anchored params viewport equal to the effective one must give
        //     output identical to passing nothing at all.
        auto plain = TETriangulationParams::Rect(rect);
        auto anchored = TETriangulationParams::Rect(rect);
        anchored.viewport = screen;

        auto plainRes = ctx.triangulateSync(plain);
        auto anchoredRes = ctx.triangulateSync(anchored);
        const Bounds pb = boundsOf(plainRes), ab = boundsOf(anchoredRes);
        assert(nearlyEqual(pb.minX, ab.minX) && nearlyEqual(pb.maxX, ab.maxX) &&
               nearlyEqual(pb.minY, ab.minY) && nearlyEqual(pb.maxY, ab.maxY) &&
               "an anchored params viewport must agree with the default path");

        // (b) params.viewport wins over the call-arg viewport.
        auto contested = TETriangulationParams::Rect(rect);
        contested.viewport = GEViewport{0.f, 0.f, 200.f, 100.f, 0.f, 1.f};  // rect exactly fills it
        GEViewport callArg{0.f, 0.f, 800.f, 600.f, 0.f, 1.f};

        auto contestedRes = ctx.triangulateSync(
            contested, GTEPolygonFrontFaceRotation::Clockwise, &callArg);
        const Bounds cb = boundsOf(contestedRes);
        // Under the params viewport the rect fills NDC exactly; under the call
        // arg it would only reach x=-0.5, y=+0.667.
        assert(nearlyEqual(cb.minX, -1.f) && nearlyEqual(cb.maxX, 1.f) &&
               "params.viewport must win over the call argument (X)");
        assert(nearlyEqual(cb.minY, -1.f) && nearlyEqual(cb.maxY, 1.f) &&
               "params.viewport must win over the call argument (Y)");

        // (c) With no params viewport, the call arg still works (back-compat).
        auto legacy = TETriangulationParams::Rect(rect);
        GEViewport tight{0.f, 0.f, 200.f, 100.f, 0.f, 1.f};
        auto legacyRes = ctx.triangulateSync(
            legacy, GTEPolygonFrontFaceRotation::Clockwise, &tight);
        const Bounds lb = boundsOf(legacyRes);
        assert(nearlyEqual(lb.minX, -1.f) && nearlyEqual(lb.maxX, 1.f) &&
               "the call-arg viewport must still be honored when params has none");
    }

    // --- 9.6: local space emits raw authored units, no bake at all. ----------
    {
        TestTEContext ctx(screen);
        GSphere sphere;
        sphere.center = GPoint3D{0.f, 0.f, 0.f};
        sphere.radius = 2.f;

        auto params = TETriangulationParams::Sphere(sphere);
        params.localSpace = true;

        auto result = ctx.triangulateSync(params);
        assert(!result.mesh.vertexPolygons.empty() && "local-space sphere produced no geometry");
        assert(allFinite(result) && "local-space sphere has non-finite vertices");

        // A radius-2 sphere spans its literal radius on every axis — and, unlike
        // the baked path, it is ROUND: X and Y have the same extent even though
        // the viewport is 800x600. That is the whole point of local space.
        const Bounds b = boundsOf(result);
        assert(nearlyEqual(b.minX, -2.f, 0.05f) && nearlyEqual(b.maxX, 2.f, 0.05f) && "local X extent = radius");
        assert(nearlyEqual(b.minY, -2.f, 0.05f) && nearlyEqual(b.maxY, 2.f, 0.05f) && "local Y extent = radius");
        assert(nearlyEqual(b.minZ, -2.f, 0.05f) && nearlyEqual(b.maxZ, 2.f, 0.05f) && "local Z extent = radius");

        const float xSpan = b.maxX - b.minX;
        const float ySpan = b.maxY - b.minY;
        assert(nearlyEqual(xSpan, ySpan, 0.05f) && "a local-space sphere must be round, not aspect-squashed");

        // Local space wins over a params viewport that is also set.
        auto both = TETriangulationParams::Sphere(sphere);
        both.localSpace = true;
        both.viewport = GEViewport{100.f, 50.f, 400.f, 200.f, 0.f, 1.f};
        const Bounds bb = boundsOf(ctx.triangulateSync(both));
        assert(nearlyEqual(bb.maxX, 2.f, 0.05f) && "localSpace must win over params.viewport");
    }

    // --- Contrast: the SAME sphere baked to NDC is elliptical, which is why
    //     local space exists. This asserts the bake still does what it says,
    //     not that the bake is what a 3D caller wants. -----------------------
    {
        TestTEContext ctx(screen);
        GSphere sphere;
        sphere.center = GPoint3D{400.f, 300.f, 0.f};
        sphere.radius = 100.f;

        const Bounds b = boundsOf(ctx.triangulateSync(TETriangulationParams::Sphere(sphere)));
        const float xSpan = b.maxX - b.minX;   // 200/800 * 2 = 0.5
        const float ySpan = b.maxY - b.minY;   // 200/600 * 2 = 0.667
        assert(nearlyEqual(xSpan, 0.5f, 0.02f) && "baked X span = 2*diameter/width");
        assert(nearlyEqual(ySpan, 0.6667f, 0.02f) && "baked Y span = 2*diameter/height");
        assert(!nearlyEqual(xSpan, ySpan, 0.02f) &&
               "the baked sphere is aspect-squashed by construction — use localSpace for 3D");
    }

    // --- Params-level winding, same precedence rule as the viewport. ---------
    {
        TestTEContext ctx(screen);
        GRect rect;
        rect.pos = GPoint2D{0.f, 0.f};
        rect.w = 200.f;
        rect.h = 100.f;

        auto cw = TETriangulationParams::Rect(rect);
        auto ccw = TETriangulationParams::Rect(rect);
        ccw.frontFaceRotation = GTEPolygonFrontFaceRotation::CounterClockwise;

        auto cwRes = ctx.triangulateSync(cw, GTEPolygonFrontFaceRotation::Clockwise);
        // The params field must win even though the call arg says Clockwise.
        auto ccwRes = ctx.triangulateSync(ccw, GTEPolygonFrontFaceRotation::Clockwise);

        assert(!cwRes.mesh.vertexPolygons.empty() && !ccwRes.mesh.vertexPolygons.empty());
        const auto &a = cwRes.mesh.vertexPolygons.front();
        const auto &b = ccwRes.mesh.vertexPolygons.front();

        // Reversing the winding swaps b/c and leaves a alone.
        assert(pointNearlyEqual(a.a.pt, b.a.pt) && "vertex a should be unchanged by winding");
        assert(pointNearlyEqual(a.b.pt, b.c.pt) && pointNearlyEqual(a.c.pt, b.b.pt) &&
               "params.frontFaceRotation=CCW must swap b/c, beating the Clockwise call arg");

        // With no params winding, the call arg still drives it (back-compat).
        auto legacy = TETriangulationParams::Rect(rect);
        auto legacyRes = ctx.triangulateSync(legacy, GTEPolygonFrontFaceRotation::CounterClockwise);
        const auto &l = legacyRes.mesh.vertexPolygons.front();
        assert(pointNearlyEqual(l.b.pt, a.c.pt) && pointNearlyEqual(l.c.pt, a.b.pt) &&
               "the call-arg winding must still be honored when params has none");
    }

    std::printf("te_coordspace_test: all checks passed\n");
    return 0;
}
