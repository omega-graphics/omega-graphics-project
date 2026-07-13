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

    // =====================================================================
    // Phase 2 — object model and transforms
    // =====================================================================

    const GEViewport screen{0.f, 0.f, 800.f, 600.f, 0.f, 1.f};

    // --- GESpaceTransform::modelMatrix composes T ∘ R ∘ S, in that order. ----
    {
        GESpaceTransform t;
        expectPoint(transformPoint(t.modelMatrix(), GPoint3D{3.f, 4.f, 5.f}),
                    3.f, 4.f, 5.f, "default transform is identity");

        t.translation = GPoint3D{10.f, 20.f, 30.f};
        expectPoint(transformPoint(t.modelMatrix(), GPoint3D{0.f, 0.f, 0.f}),
                    10.f, 20.f, 30.f, "translation only");

        GESpaceTransform s;
        s.scale = GPoint3D{2.f, 3.f, 4.f};
        expectPoint(transformPoint(s.modelMatrix(), GPoint3D{1.f, 1.f, 1.f}),
                    2.f, 3.f, 4.f, "scale only");

        GESpaceTransform r;
        r.rotation = FQuaternion::fromAxisAngle(0.f, 0.f, 1.f, HalfPi<float>);
        expectPoint(transformPoint(r.modelMatrix(), GPoint3D{1.f, 0.f, 0.f}),
                    0.f, 1.f, 0.f, "rotation only (+90 about Z)");

        // ORDER. Scale, then rotate, then translate. A point at (1,0,0):
        //   scale x2      -> (2,0,0)
        //   rotate +90 Z  -> (0,2,0)
        //   translate +10 -> (10,2,0)
        // If the composition were transposed (translate first), the translation
        // would itself be scaled and rotated and land somewhere else entirely —
        // this is the assertion that catches GTE's reversed operator*.
        GESpaceTransform trs;
        trs.scale = GPoint3D{2.f, 2.f, 2.f};
        trs.rotation = FQuaternion::fromAxisAngle(0.f, 0.f, 1.f, HalfPi<float>);
        trs.translation = GPoint3D{10.f, 0.f, 0.f};
        expectPoint(transformPoint(trs.modelMatrix(), GPoint3D{1.f, 0.f, 0.f}),
                    10.f, 2.f, 0.f, "model matrix applies scale, then rotation, then translation");

        // Scale must NOT move the object: an object at the origin stays at the
        // origin no matter how it is scaled.
        GESpaceTransform big;
        big.translation = GPoint3D{5.f, 5.f, 5.f};
        big.scale = GPoint3D{100.f, 100.f, 100.f};
        expectPoint(transformPoint(big.modelMatrix(), GPoint3D{0.f, 0.f, 0.f}),
                    5.f, 5.f, 5.f, "scale must not scale the translation");
    }

    // --- Quaternion rotation agrees with the Euler matrix builder. -----------
    {
        const float pitch = 0.3f, yaw = -0.7f, roll = 1.1f;
        const auto viaQuat = FQuaternion::fromEuler(pitch, yaw, roll).toMatrix();
        const auto viaMatrix = rotationEuler(pitch, yaw, roll);

        const GPoint3D probes[] = {
            {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}, {1.f, -2.f, 3.f},
        };
        for (const auto &p : probes) {
            const auto a = transformPoint(viaQuat, p);
            const auto b = transformPoint(viaMatrix, p);
            if (!pointNearlyEqual(a, b, 1e-4f)) {
                std::printf("FAIL fromEuler/rotationEuler disagree at (%f,%f,%f): "
                            "quat (%f,%f,%f) vs matrix (%f,%f,%f)\n",
                            p.x, p.y, p.z, a.x, a.y, a.z, b.x, b.y, b.z);
                assert(false && "FQuaternion::fromEuler must match rotationEuler");
            }
        }
    }

    // --- The object table: handles, mutators, accumulation. ------------------
    {
        GESpace space(screen);

        const auto id = space.addObject();
        assert(id != GESpaceInvalidObject && "addObject must mint a valid handle");
        assert(space.contains(id) && "the space should contain what it just added");
        assert(!space.contains(GESpaceInvalidObject) && "the invalid handle names nothing");
        assert(!space.contains(id + 1000) && "an unknown handle names nothing");

        // Handles are distinct.
        const auto other = space.addObject();
        assert(other != id && "handles must be unique");

        // translate() accumulates; setTranslation() replaces.
        space.translate(id, 1.f, 2.f, 3.f);
        space.translate(id, 10.f, 20.f, 30.f);
        expectPoint(space.transformOf(id).translation, 11.f, 22.f, 33.f, "translate accumulates");
        space.setTranslation(id, GPoint3D{-1.f, -2.f, -3.f});
        expectPoint(space.transformOf(id).translation, -1.f, -2.f, -3.f, "setTranslation replaces");

        // scale() accumulates multiplicatively; setScale() replaces.
        space.scale(id, 2.f, 2.f, 2.f);
        space.scale(id, 3.f, 3.f, 3.f);
        expectPoint(space.transformOf(id).scale, 6.f, 6.f, 6.f, "scale accumulates multiplicatively");
        space.setScale(id, GPoint3D{1.f, 1.f, 1.f});
        expectPoint(space.transformOf(id).scale, 1.f, 1.f, 1.f, "setScale replaces");

        // rotate() COMPOSES: two 45° turns about Z equal one 90° turn.
        const auto spun = space.addObject();
        space.rotate(spun, 0.f, 0.f, HalfPi<float> * 0.5f);
        space.rotate(spun, 0.f, 0.f, HalfPi<float> * 0.5f);
        expectPoint(transformPoint(space.transformOf(spun).rotation.toMatrix(), GPoint3D{1.f, 0.f, 0.f}),
                    0.f, 1.f, 0.f, "two 45deg rotations compose into one 90deg rotation");

        // ...and it stays a pure rotation. 64 accumulated turns must not drift
        // off the unit quaternion — that drift is what would silently introduce
        // a scale into the model matrix.
        const auto drifted = space.addObject();
        for (int i = 0; i < 64; i++) {
            space.rotateAxis(drifted, 0.f, 0.f, 1.f, TwoPi<float> / 64.f);
        }
        const auto q = space.transformOf(drifted).rotation;
        assert(nearlyEqual(q.length(), 1.f, 1e-4f) && "accumulated rotation must stay unit-length");
        // 64 x (360/64) = a full turn, back to where it started.
        expectPoint(transformPoint(q.toMatrix(), GPoint3D{1.f, 0.f, 0.f}),
                    1.f, 0.f, 0.f, "64 accumulated turns come full circle without drift");

        // rotateAxis on a zero-length axis is a no-op, not a NaN.
        const auto degenerate = space.addObject();
        space.rotateAxis(degenerate, 0.f, 0.f, 0.f, 1.f);
        const auto dq = space.transformOf(degenerate).rotation;
        assert(!std::isnan(dq.x) && !std::isnan(dq.w) && "a zero axis must not produce NaN");
        assert(nearlyEqual(dq.length(), 1.f) && "a zero axis must leave the rotation untouched");
    }

    // --- objectTransform = spaceToNDC ∘ model, and it IS the final result. ---
    {
        GESpace space(screen);
        const auto id = space.addObject();
        space.setTranslation(id, GPoint3D{400.f, 300.f, 0.f});
        space.setScale(id, GPoint3D{100.f, 100.f, 1.f});
        space.rotateAxis(id, 0.f, 0.f, 1.f, HalfPi<float>);

        // Hand-compose the same thing and require an exact agreement, pointwise.
        const auto model = space.transformOf(id).modelMatrix();
        const auto ndc = space.spaceToNDC();
        const GPoint3D probes[] = {
            {0.f, 0.f, 0.f}, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {-1.f, -1.f, 0.5f},
        };
        for (const auto &p : probes) {
            const auto viaObject = transformPoint(space.objectTransform(id), p);
            const auto byHand = transformPoint(ndc, transformPoint(model, p));
            if (!pointNearlyEqual(viaObject, byHand, 1e-4f)) {
                std::printf("FAIL objectTransform != spaceToNDC(model(p)) at (%f,%f,%f): "
                            "(%f,%f,%f) vs (%f,%f,%f)\n",
                            p.x, p.y, p.z, viaObject.x, viaObject.y, viaObject.z,
                            byHand.x, byHand.y, byHand.z);
                assert(false && "objectTransform must equal spaceToNDC applied after the model matrix");
            }
        }

        // The object sits at the viewport center, so its local origin lands at
        // NDC (0,0) — the end-to-end statement of "place it, then draw it".
        expectPoint(transformPoint(space.objectTransform(id), GPoint3D{0.f, 0.f, 0.f}),
                    0.f, 0.f, 0.f, "an object at the viewport center maps to NDC origin");
    }

    // --- The regression the old TEMesh::rotate failed: a 90deg rotation in a
    //     NON-SQUARE viewport must keep a square square in SPACE units, and pick
    //     up aspect scaling only on the way through spaceToNDC. ---------------
    {
        GESpace space(screen);   // 800x600 — deliberately non-square
        const auto id = space.addObject();
        space.setTranslation(id, GPoint3D{400.f, 300.f, 0.f});
        space.setScale(id, GPoint3D{100.f, 100.f, 1.f});

        // A unit square's corners, in the object's local space.
        const GPoint3D corners[4] = {
            {-1.f, -1.f, 0.f}, {1.f, -1.f, 0.f}, {1.f, 1.f, 0.f}, {-1.f, 1.f, 0.f},
        };

        auto spaceExtent = [&](GPoint3D *out) {
            const auto model = space.transformOf(id).modelMatrix();
            for (int i = 0; i < 4; i++) out[i] = transformPoint(model, corners[i]);
        };

        GPoint3D before[4], after[4];
        spaceExtent(before);
        space.rotateAxis(id, 0.f, 0.f, 1.f, HalfPi<float>);
        spaceExtent(after);

        auto sideLength = [](const GPoint3D &a, const GPoint3D &b) {
            const float dx = b.x - a.x, dy = b.y - a.y;
            return std::sqrt(dx*dx + dy*dy);
        };

        // In SPACE units the rotated square is still a square: all four sides
        // equal, and equal to what they were before the rotation. The old
        // TEMesh::rotate spun vertices in NDC and turned this into a rectangle.
        for (int i = 0; i < 4; i++) {
            const float s0 = sideLength(before[i], before[(i + 1) % 4]);
            const float s1 = sideLength(after[i], after[(i + 1) % 4]);
            assert(nearlyEqual(s0, 200.f, 0.01f) && "unrotated square side should be 2*scale");
            assert(nearlyEqual(s1, 200.f, 0.01f) &&
                   "a rotation in space units must NOT distort the square under a non-square viewport");
        }

        // The aspect stretch appears only after spaceToNDC — and it is the
        // viewport's, not the rotation's: x is divided by 800, y by 600.
        const auto ndcA = transformPoint(space.objectTransform(id), corners[0]);
        const auto ndcB = transformPoint(space.objectTransform(id), corners[1]);
        const float ndcSideX = std::fabs(ndcB.x - ndcA.x);
        const float ndcSideY = std::fabs(ndcB.y - ndcA.y);
        // After a 90deg turn that edge runs vertically, so it spans 200 space
        // units of Y => 2*200/600 in NDC, and none of X.
        assert(nearlyEqual(ndcSideX, 0.f, 1e-3f) && "the rotated edge should be vertical in NDC");
        assert(nearlyEqual(ndcSideY, 2.f * 200.f / 600.f, 1e-3f) &&
               "aspect scaling must come from spaceToNDC (y/600), not from the rotation");
    }

    // --- Unknown handles degrade loudly, never into garbage matrices. --------
    {
        GESpace space(screen);
        const GESpaceObjectID ghost = 4242;

        space.translate(ghost, 1.f, 1.f, 1.f);   // logs, does nothing
        space.rotate(ghost, 1.f, 1.f, 1.f);
        space.scale(ghost, 2.f, 2.f, 2.f);

        const auto &t = space.transformOf(ghost);
        expectPoint(t.translation, 0.f, 0.f, 0.f, "unknown handle yields an identity transform");
        expectPoint(t.scale, 1.f, 1.f, 1.f, "unknown handle yields unit scale");

        // objectTransform on a ghost falls back to the bare space matrix, so a
        // caller that ignores the error still draws something sane.
        const auto ghostM = space.objectTransform(ghost);
        const auto spaceM = space.spaceToNDC();
        const bool same = (ghostM == spaceM);
        assert(same && "objectTransform on an unknown handle should fall back to spaceToNDC");
    }

    std::printf("gespace_test: all checks passed\n");
    return 0;
}
