/// GESpace-Implementation-Plan Phase 1 — backend-independent unit test for
/// `GESpace` and its space→NDC matrix. Pure CPU: GESpace is a transform
/// authority, not a GPU resource, so no device or triangulation context is
/// needed (mirrors te_index_buffer_test.cpp).
///
/// The suite pins down three things that were empirically wrong or ambiguous
/// before this phase:
///   1. The mapping itself — origin-aware, Y-flipped, [0,1] depth.
///   2. The column-major memory layout, which is what lets the matrix be
///      memcpy'd straight into a uniform / push constant.
///   3. `transformPoint()`, which used to apply the TRANSPOSE of a column-major
///      matrix (silently dropping translation and inverting rotation).

#include <omegaGTE/GESpace.h>
#include <omegaGTE/GTEMath.h>

#include <cassert>
#include <cmath>
#include <cstdio>

using namespace OmegaGTE;

namespace {

constexpr float kEps = 1e-5f;

bool nearlyEqual(float a, float b, float eps = kEps) {
    return std::fabs(a - b) <= eps;
}

bool pointNearlyEqual(const GPoint3D &a, const GPoint3D &b, float eps = kEps) {
    return nearlyEqual(a.x, b.x, eps) && nearlyEqual(a.y, b.y, eps) && nearlyEqual(a.z, b.z, eps);
}

void expectPoint(const GPoint3D &got, float x, float y, float z, const char *what) {
    const GPoint3D want{x, y, z};
    if (!pointNearlyEqual(got, want)) {
        std::printf("FAIL %s: got (%f, %f, %f), want (%f, %f, %f)\n",
                    what, got.x, got.y, got.z, want.x, want.y, want.z);
        assert(false && "GESpace point mapping mismatch");
    }
}

/// The X/Y half of `OmegaTriangulationEngineContext::translateCoordsDefaultImpl`
/// (gte/src/TE.cpp:351), reproduced here because that method is `protected` on
/// an abstract, device-bound class and cannot be called from a unit test.
/// GESpace's X and Y must agree with it exactly for an origin-anchored
/// viewport; its Z branch is deliberately NOT reproduced (TE bakes depth to the
/// OpenGL-style [-1,1] range, GESpace maps to the [0,1] range the backends
/// clip against — see GESpace::spaceToNDC).
void teTranslateCoordsXY(float x, float y, const GEViewport &vp, float *xr, float *yr) {
    *xr = (2.f * x / vp.width) - 1.f;
    *yr = 1.f - (2.f * y / vp.height);
}

}  // namespace

int main() {
    // --- The mapping: origin-anchored, non-square viewport. -----------------
    {
        const GEViewport vp{0.f, 0.f, 800.f, 600.f, 0.f, 1.f};
        GESpace space(vp);
        const auto m = space.spaceToNDC();

        // Corners. Top-left of a Y-down viewport is NDC (-1, +1); bottom-right
        // is (+1, -1).
        expectPoint(transformPoint(m, GPoint3D{0.f, 0.f, 0.f}), -1.f, 1.f, 0.f, "top-left corner");
        expectPoint(transformPoint(m, GPoint3D{800.f, 600.f, 0.f}), 1.f, -1.f, 0.f, "bottom-right corner");
        expectPoint(transformPoint(m, GPoint3D{400.f, 300.f, 0.f}), 0.f, 0.f, 0.f, "viewport center");

        // Depth maps to [0,1] — the range Vulkan / D3D12 / Metal clip against.
        expectPoint(transformPoint(m, GPoint3D{400.f, 300.f, 1.f}), 0.f, 0.f, 1.f, "far plane -> z_ndc 1");
        expectPoint(transformPoint(m, GPoint3D{400.f, 300.f, 0.5f}), 0.f, 0.f, 0.5f, "mid depth -> z_ndc 0.5");

        // X/Y agree with TE's translateCoordsDefaultImpl at arbitrary points.
        const GPoint3D samples[] = {
            {0.f, 0.f, 0.f}, {123.f, 456.f, 0.f}, {800.f, 0.f, 0.f}, {17.5f, 599.f, 0.f},
        };
        for (const auto &pt : samples) {
            float wantX = 0.f, wantY = 0.f;
            teTranslateCoordsXY(pt.x, pt.y, vp, &wantX, &wantY);
            const auto got = transformPoint(m, pt);
            if (!nearlyEqual(got.x, wantX) || !nearlyEqual(got.y, wantY)) {
                std::printf("FAIL translateCoords parity at (%f, %f): got (%f, %f), want (%f, %f)\n",
                            pt.x, pt.y, got.x, got.y, wantX, wantY);
                assert(false && "spaceToNDC disagrees with translateCoordsDefaultImpl on X/Y");
            }
        }
    }

    // --- Origin awareness: the bug `viewportMatrix()` has. A viewport anchored
    //     at (100, 50) must map its OWN top-left to NDC (-1, +1), not the
    //     world origin. -------------------------------------------------------
    {
        const GEViewport vp{100.f, 50.f, 400.f, 200.f, 0.f, 1.f};
        GESpace space(vp);
        const auto m = space.spaceToNDC();

        expectPoint(transformPoint(m, GPoint3D{100.f, 50.f, 0.f}), -1.f, 1.f, 0.f, "offset viewport top-left");
        expectPoint(transformPoint(m, GPoint3D{500.f, 250.f, 0.f}), 1.f, -1.f, 0.f, "offset viewport bottom-right");
        expectPoint(transformPoint(m, GPoint3D{300.f, 150.f, 0.f}), 0.f, 0.f, 0.f, "offset viewport center");

        // The world origin is now OUTSIDE the space, and must map outside NDC.
        const auto origin = transformPoint(m, GPoint3D{0.f, 0.f, 0.f});
        assert(origin.x < -1.f && "world origin should fall left of an offset viewport");
        assert(origin.y > 1.f && "world origin should fall above an offset viewport");
    }

    // --- Non-zero near depth: near -> 0, far -> 1. ---------------------------
    {
        const GEViewport vp{0.f, 0.f, 100.f, 100.f, 10.f, 110.f};
        GESpace space(vp);
        const auto m = space.spaceToNDC();

        expectPoint(transformPoint(m, GPoint3D{50.f, 50.f, 10.f}), 0.f, 0.f, 0.f, "near plane -> z_ndc 0");
        expectPoint(transformPoint(m, GPoint3D{50.f, 50.f, 110.f}), 0.f, 0.f, 1.f, "far plane -> z_ndc 1");
        expectPoint(transformPoint(m, GPoint3D{50.f, 50.f, 60.f}), 0.f, 0.f, 0.5f, "mid plane -> z_ndc 0.5");
    }

    // --- Column-major layout. The header promises an FMatrix<4,4> can be
    //     memcpy'd into a uniform / push constant; that only holds if column c
    //     is contiguous and the translation sits in floats 12..14. ------------
    {
        const GEViewport vp{0.f, 0.f, 800.f, 600.f, 0.f, 1.f};
        GESpace space(vp);
        const auto m = space.spaceToNDC();
        const float *raw = m.data();

        for (unsigned c = 0; c < 4; ++c) {
            for (unsigned r = 0; r < 4; ++r) {
                assert(nearlyEqual(raw[c * 4 + r], m[c][r]) && "FMatrix storage is not column-major");
            }
        }
        // Translation column: x offset -1, y offset +1, z offset 0 (near = 0).
        assert(nearlyEqual(raw[12], -1.f) && "column 3 row 0 should carry the X offset");
        assert(nearlyEqual(raw[13], 1.f) && "column 3 row 1 should carry the Y offset");
        assert(nearlyEqual(raw[14], 0.f) && "column 3 row 2 should carry the Z offset");
    }

    // --- setViewport re-anchors the space (window resize). ------------------
    {
        GESpace space(GEViewport{0.f, 0.f, 800.f, 600.f, 0.f, 1.f});
        expectPoint(transformPoint(space.spaceToNDC(), GPoint3D{800.f, 600.f, 0.f}),
                    1.f, -1.f, 0.f, "pre-resize bottom-right");

        const GEViewport resized{0.f, 0.f, 1920.f, 1080.f, 0.f, 1.f};
        space.setViewport(resized);
        assert(nearlyEqual(space.viewport().width, 1920.f) && "viewport() should report the new viewport");
        expectPoint(transformPoint(space.spaceToNDC(), GPoint3D{1920.f, 1080.f, 0.f}),
                    1.f, -1.f, 0.f, "post-resize bottom-right");
        // The old extent no longer reaches the corner.
        const auto stale = transformPoint(space.spaceToNDC(), GPoint3D{800.f, 600.f, 0.f});
        assert(stale.x < 1.f && stale.y > -1.f && "resize should have rescaled the space");
    }

    // --- Degenerate viewport: loud identity, never NaN. A zero-height viewport
    //     is real (a minimized window), and a NaN matrix in a draw call is far
    //     worse to debug than an identity one. --------------------------------
    {
        GESpace space(GEViewport{0.f, 0.f, 800.f, 0.f, 0.f, 1.f});
        const auto m = space.spaceToNDC();
        // Hoisted: `FMatrix<4,4>` inside assert() would split on the comma.
        const bool isIdentity = (m == FMatrix<4,4>::Identity());
        assert(isIdentity && "degenerate viewport should yield identity");

        const auto pt = transformPoint(m, GPoint3D{1.f, 2.f, 3.f});
        assert(!std::isnan(pt.x) && !std::isnan(pt.y) && !std::isnan(pt.z) && "identity must not produce NaN");

        // Recovering from the degenerate state must restore a real mapping.
        space.setViewport(GEViewport{0.f, 0.f, 800.f, 600.f, 0.f, 1.f});
        expectPoint(transformPoint(space.spaceToNDC(), GPoint3D{400.f, 300.f, 0.f}),
                    0.f, 0.f, 0.f, "recovered viewport center");
    }

    // --- Regression guard: transformPoint() honors the column-major
    //     convention. Before the Phase 1 fix it applied M^T, which dropped the
    //     translation column entirely and rotated backwards. ------------------
    {
        expectPoint(transformPoint(translationMatrix(1.f, 2.f, 3.f), GPoint3D{0.f, 0.f, 0.f}),
                    1.f, 2.f, 3.f, "translationMatrix must translate");

        // +90 deg about Z takes (1,0,0) to (0,1,0) under the column-vector
        // convention. The transposed (broken) path returned (0,-1,0).
        expectPoint(transformPoint(rotationZ(HalfPi<float>), GPoint3D{1.f, 0.f, 0.f}),
                    0.f, 1.f, 0.f, "rotationZ(+90) must rotate counter-clockwise");

        expectPoint(transformPoint(scalingMatrix(2.f, 3.f, 4.f), GPoint3D{1.f, 1.f, 1.f}),
                    2.f, 3.f, 4.f, "scalingMatrix must scale");
    }

    std::printf("gespace_test: all checks passed\n");
    return 0;
}
