/// GESpace-Implementation-Plan Phases 1-3 — backend-independent unit test for
/// `GESpace`, its space→NDC matrix, and mesh placement. Pure CPU: GESpace is a
/// transform authority, not a GPU resource, so no device or triangulation
/// context is needed (mirrors te_index_buffer_test.cpp).
///
/// The suite pins down three things that were empirically wrong or ambiguous
/// before this phase:
///   1. The mapping itself — origin-aware, Y-flipped, [0,1] depth.
///   2. The column-major memory layout, which is what lets the matrix be
///      memcpy'd straight into a uniform / push constant.
///   3. `transformPoint()`, which used to apply the TRANSPOSE of a column-major
///      matrix (silently dropping translation and inverting rotation).
///
/// Phase 3 adds mesh placement: the mesh is referenced rather than copied, one
/// mesh can be placed twice with independent transforms, handles are retired
/// and never recycled, and `GEMesh::bounds` makes fitting an asset to a
/// viewport arithmetic instead of a magic number.

#include <omegaGTE/GESpace.h>
#include <omegaGTE/GEMesh.h>
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

    // =====================================================================
    // Phase 3 — placing GEMeshes.
    //
    // A GEMesh is a plain resource holder, so a default-constructed one (null
    // buffers, zero counts) is a perfectly good stand-in here: Phase 3 places
    // and transforms the HANDLE, and never reads a vertex. The GPU-side proof
    // that the composed matrix actually puts geometry on screen is
    // MeshAndRaytracingTest, not this unit test.
    // =====================================================================

    // --- The mesh is referenced, not copied. --------------------------------
    {
        GESpace space(screen);
        auto mesh = std::make_shared<GEMesh>();
        mesh->vertexCount = 300;

        const auto id = space.addMesh(mesh);
        assert(id != GESpaceInvalidObject && "addMesh should mint a live handle");
        assert(space.contains(id));

        auto got = space.meshOf(id);
        assert(got == mesh && "meshOf must hand back the same GEMesh, not a copy");
        assert(got->vertexCount == 300u);
        assert(mesh.use_count() > 1 && "the space must hold a reference to the mesh");
    }

    // --- One mesh, two placements: shared geometry, independent transforms. --
    // This is the instancing contract. If the transforms were not independent
    // (e.g. if GESpace keyed anything off the mesh pointer) moving one copy
    // would drag the other.
    {
        GESpace space(screen);
        auto mesh = std::make_shared<GEMesh>();

        const auto left  = space.addMesh(mesh);
        const auto right = space.addMesh(mesh);
        assert(left != right && "two placements of one mesh are two distinct objects");
        assert(space.meshOf(left) == space.meshOf(right) && "both share the one GPU buffer");

        space.setTranslation(left,  GPoint3D{100.f, 300.f, 0.f});
        space.setTranslation(right, GPoint3D{700.f, 300.f, 0.f});

        const auto origin = GPoint3D{0.f, 0.f, 0.f};
        // x=100 of 800 → NDC -0.75; x=700 → +0.75. Both at y=300 of 600 → 0.
        expectPoint(transformPoint(space.objectTransform(left), origin),
                    -0.75f, 0.f, 0.f, "left instance lands at its own translation");
        expectPoint(transformPoint(space.objectTransform(right), origin),
                    0.75f, 0.f, 0.f, "right instance is unaffected by the left one");
    }

    // --- A transform-only object simply has no mesh. -------------------------
    {
        GESpace space(screen);
        const auto anchor = space.addObject();
        assert(space.contains(anchor));
        assert(space.meshOf(anchor) == nullptr &&
               "addObject places a pure transform node — no geometry, and that is not an error");

        // A null mesh is refused outright rather than becoming a live handle
        // that transforms nothing and draws a blank frame.
        const auto bad = space.addMesh(nullptr);
        assert(bad == GESpaceInvalidObject && "addMesh(null) must not mint a handle");
        assert(!space.contains(bad));
    }

    // --- objects() enumerates in insertion order; remove() retires a handle. -
    {
        GESpace space(screen);
        auto mesh = std::make_shared<GEMesh>();

        const auto a = space.addMesh(mesh);
        const auto b = space.addObject();
        const auto c = space.addMesh(mesh);

        auto ids = space.objects();
        assert(ids.size() == 3);
        assert(ids[0] == a && ids[1] == b && ids[2] == c &&
               "objects() must enumerate in insertion order so draws are stable frame to frame");

        space.remove(b);
        ids = space.objects();
        assert(ids.size() == 2);
        assert(ids[0] == a && ids[1] == c);
        assert(!space.contains(b));
        assert(space.meshOf(b) == nullptr);

        // The retired handle is never recycled — a later add must not silently
        // inherit it, or a caller holding the stale ID would start addressing
        // someone else's object.
        const auto d = space.addMesh(mesh);
        assert(d != b && "handles are retired, never reused");
        assert(space.contains(a) && space.contains(c) && space.contains(d));

        // Dropping the last placement drops the space's reference to the mesh.
        space.remove(a);
        space.remove(c);
        space.remove(d);
        assert(space.objects().empty());
        assert(mesh.use_count() == 1 && "remove() must release the space's mesh reference");
    }

    // --- Mesh bounds: the local-space extent that makes placement non-magic. -
    // Without these, fitting a loaded asset into a viewport is a hand-tuned
    // constant. Position is the first float3 of each vertex, whatever else the
    // layout carries — so a Position+Normal stride must skip the normals.
    {
        // Two vertices, Position(3) + Normal(3) = 24B stride. The normals are
        // deliberately far outside the position range: if the reader ever
        // strides wrong, they poison the box and this fails loudly.
        const float packed[] = {
            -2.f, -4.f, -6.f,   100.f, 100.f, 100.f,
             8.f,  4.f,  2.f,  -100.f, -100.f, -100.f,
        };
        const auto b = geMeshComputeBounds(packed, 2, sizeof(float) * 6);
        assert(b.valid);
        expectPoint(b.min, -2.f, -4.f, -6.f, "bounds min reads only the Position attribute");
        expectPoint(b.max, 8.f, 4.f, 2.f, "bounds max reads only the Position attribute");
        expectPoint(b.center(), 3.f, 0.f, -2.f, "bounds center");
        expectPoint(b.extent(), 10.f, 8.f, 8.f, "bounds extent");
        assert(nearlyEqual(b.longestExtent(), 10.f) && "longestExtent is the largest axis");

        // A mesh whose vertices are all negative must NOT have its box
        // stretched to the origin — that is what a zero-initialized min/max
        // would do, and it would halve the fit scale of any such asset.
        const float allNegative[] = {-5.f, -5.f, -5.f, -3.f, -3.f, -3.f};
        const auto nb = geMeshComputeBounds(allNegative, 2, sizeof(float) * 3);
        expectPoint(nb.min, -5.f, -5.f, -5.f, "an all-negative mesh keeps its true min");
        expectPoint(nb.max, -3.f, -3.f, -3.f, "an all-negative mesh must not be stretched to 0");

        // Degenerate inputs report invalid rather than a zero-size box a caller
        // would divide by.
        assert(!geMeshComputeBounds(nullptr, 4, 12).valid);
        assert(!geMeshComputeBounds(packed, 0, 12).valid && "no vertices means no bounds");
        assert(!geMeshComputeBounds(packed, 2, 8).valid && "a stride too small for a float3 has no position");
        assert(!GEMesh().bounds.valid && "a freshly-constructed mesh has no bounds yet");
    }

    // --- Fit-to-viewport: the workflow MeshAndRaytracingTest actually uses. --
    // Bounds → scale → center. An asset authored in arbitrary units lands
    // centered in the viewport, scaled to fill a chosen fraction of it, with no
    // per-asset magic number anywhere.
    //
    // Note the depth range on the space viewport. A GESpace viewport's
    // nearDepth/farDepth are in SPACE UNITS, exactly like width/height — they
    // are not the [0,1] the rasterizer viewport wants. Reuse the rasterizer's
    // {near=0, far=1} here and a uniform fit scale (sized in pixels, so tens or
    // hundreds) throws Z hundreds of units past the far plane and the whole mesh
    // clips away — a blank screen with every matrix "correct". A pixel-space
    // viewport needs a pixel-scaled depth range.
    {
        GEViewport spaceVp;
        spaceVp.x = 0.f;   spaceVp.y = 0.f;
        spaceVp.width = 800.f;  spaceVp.height = 600.f;
        spaceVp.nearDepth = -1000.f;  spaceVp.farDepth = 1000.f;

        GESpace space(spaceVp);
        auto mesh = std::make_shared<GEMesh>();
        // A 40-unit-wide model sitting way off its own origin, on every axis —
        // the case a scale-only fit would push straight off the screen (in X/Y)
        // and straight through the far plane (in Z).
        mesh->bounds.min = GPoint3D{1000.f, 1000.f, 1000.f};
        mesh->bounds.max = GPoint3D{1040.f, 1020.f, 1010.f};
        mesh->bounds.valid = true;

        const auto id = space.addMesh(mesh);

        const float fill = 0.8f;
        const float fit = fill * 600.f / mesh->bounds.longestExtent();   // 0.8*600/40 = 12
        assert(nearlyEqual(fit, 12.f));

        // Center on ALL THREE axes. The model's center goes to the middle of the
        // viewport in X/Y and to z = 0 — the midpoint of the [-1000, 1000] depth
        // range, i.e. NDC depth 0.5.
        const auto c = mesh->bounds.center();
        space.setScale(id, GPoint3D{fit, fit, fit});
        space.setTranslation(id, GPoint3D{400.f - c.x * fit,
                                          300.f - c.y * fit,
                                          0.f   - c.z * fit});

        // The model's own center must land dead center of the viewport, at NDC
        // depth 0.5 — TRS puts scale before translation, so the translation is
        // NOT itself scaled, which is the whole reason this arithmetic is legal.
        expectPoint(transformPoint(space.objectTransform(id), c),
                    0.f, 0.f, 0.5f, "the model's center fits to the viewport center");

        // And its widest axis spans exactly `fill` of the viewport's NDC width.
        const GPoint3D xmin{mesh->bounds.min.x, c.y, c.z};
        const GPoint3D xmax{mesh->bounds.max.x, c.y, c.z};
        const auto l = transformPoint(space.objectTransform(id), xmin);
        const auto r = transformPoint(space.objectTransform(id), xmax);
        // 40 units * 12 = 480 space units of an 800-wide viewport => 2*480/800.
        assert(nearlyEqual(std::fabs(r.x - l.x), 2.f * 480.f / 800.f, 1e-4f) &&
               "the fitted mesh spans the intended fraction of the viewport");

        // Every corner of the fitted model must be inside the clip volume:
        // x,y in [-1,1] and — the one that silently kills a draw — z in [0,1].
        for (int corner = 0; corner < 8; ++corner) {
            const GPoint3D p{(corner & 1) ? mesh->bounds.max.x : mesh->bounds.min.x,
                             (corner & 2) ? mesh->bounds.max.y : mesh->bounds.min.y,
                             (corner & 4) ? mesh->bounds.max.z : mesh->bounds.min.z};
            const auto ndc = transformPoint(space.objectTransform(id), p);
            assert(ndc.x >= -1.f && ndc.x <= 1.f && "fitted mesh must be inside the clip volume in X");
            assert(ndc.y >= -1.f && ndc.y <= 1.f && "fitted mesh must be inside the clip volume in Y");
            assert(ndc.z >= 0.f && ndc.z <= 1.f &&
                   "fitted mesh must be inside the [0,1] depth clip volume — a space viewport "
                   "with a [0,1] depth range would fail this and render nothing");
        }
    }

    std::printf("gespace_test: all checks passed\n");
    return 0;
}
