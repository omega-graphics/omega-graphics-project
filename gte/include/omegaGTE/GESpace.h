#include "GTEBase.h"
#include "GTEMath.h"
#include "GE.h"
#include "GEMesh.h"

#ifndef OMEGAGTE_GESPACE_H
#define OMEGAGTE_GESPACE_H

_NAMESPACE_BEGIN_

    /// @brief A stable handle to an object placed in a GESpace, returned by
    /// `addObject()` / `addMesh()`. Handles are never reused within a space —
    /// not even after `remove()` — so a stale handle reads as invalid rather
    /// than silently addressing whatever object took its slot.
    typedef uint32_t GESpaceObjectID;

    /// @brief The handle value that never names an object. Returned by failed
    /// placements; `addObject()` mints IDs starting at 1.
    constexpr GESpaceObjectID GESpaceInvalidObject = 0;

    /// @brief An object's local→space transform, stored as TRS components.
    ///
    /// The components are the authority, not a matrix: repeated `rotate()` calls
    /// compose quaternions rather than multiplying matrices, so orientation does
    /// not accumulate drift or shear. Everything here is in **space units** —
    /// never NDC — which is what makes a rotation actually rotate instead of
    /// skewing under a non-square viewport (the defect in the old
    /// `TEMesh::rotate`, which spun vertices in clip space).
    struct OMEGAGTE_EXPORT GESpaceTransform {
        /// Space units.
        GPoint3D translation {0.f, 0.f, 0.f};
        FQuaternion rotation = FQuaternion::Identity();
        GPoint3D scale {1.f, 1.f, 1.f};

        /// @brief The composed local→space matrix: translate ∘ rotate ∘ scale.
        ///
        /// Scale is applied first, then rotation, then translation — so an
        /// object scales about its own origin and orbits nothing. Column-major,
        /// like every matrix GESpace produces (see GESpace's class docs).
        OMEGA_NODISCARD FMatrix<4,4> modelMatrix() const;
    };

    /// @brief A coordinate space that places geometry into a viewport-defined
    /// relative space and hands back the transform that maps it to NDC.
    ///
    /// GESpace owns exactly one conversion: relative/world **space units** →
    /// normalized device coordinates, derived from a `GEViewport`. Geometry is
    /// never re-baked on the CPU — a GEMesh's vertex buffer stays in its own
    /// local space, and the composed matrix is what the caller's draw pipeline
    /// applies. Phase 1 supplies the space itself; object placement and
    /// per-object transforms arrive in Phases 2-4 (see
    /// `.plans/GESpace-Implementation-Plan.md`).
    ///
    /// @paragraph Matrix convention. Every matrix GESpace produces is
    /// **column-major**, laid out the way the backends upload it: element
    /// (row r, column c) is `m[c][r]`, translation lives in column 3
    /// (`m[3][0..2]`), and a shader consumes it as `mvp * float4(pos, 1.0)`.
    /// This matches `translationMatrix` / `orthographicProjection` in GTEMath.h
    /// and `Kreate::Mat4`'s `float[16]` layout, so an `FMatrix<4,4>` from here
    /// can be memcpy'd straight into a uniform or push constant.
    class OMEGAGTE_EXPORT GESpace {
    public:
        OMEGACOMMON_CLASS("OmegaGTE.GESpace")

        /// @brief Construct a space whose relative units map to NDC through
        /// `viewport` (origin + width/height + near/far depth). The viewport is
        /// copied by value; call `setViewport()` to re-anchor it (e.g. on a
        /// window resize).
        explicit GESpace(const GEViewport & viewport);
        ~GESpace();

        GESpace(const GESpace &) = delete;
        GESpace & operator=(const GESpace &) = delete;

        /// @brief Re-anchor the space onto a new viewport. Invalidates the
        /// cached space→NDC matrix; the next `spaceToNDC()` recomposes it.
        void setViewport(const GEViewport & viewport);

        /// @brief The viewport this space is currently anchored to.
        OMEGA_NODISCARD const GEViewport & viewport() const;

        /// @brief The linear space→NDC transform derived from the current
        /// viewport. This is GESpace's canonical coordinate conversion.
        ///
        /// It is an origin-aware orthographic map honoring `GEViewport::x` and
        /// `y` (which `viewportMatrix()` in GTEMath.h does not — that one is
        /// scale-only, and is *not* what this uses):
        ///
        ///   x_ndc = 2*(x - vp.x)/vp.width - 1
        ///   y_ndc = 1 - 2*(y - vp.y)/vp.height        (Y-flip: y=0 → +1, the
        ///                                              top-left / Y-down
        ///                                              convention GEViewport
        ///                                              already documents)
        ///   z_ndc = (z - vp.nearDepth)/(vp.farDepth - vp.nearDepth)
        ///
        /// @paragraph Depth range. Z maps to **[0,1]**, the range Vulkan, D3D12
        /// and Metal actually clip against — near→0, far→1. This deliberately
        /// differs from `OmegaTriangulationEngineContext::translateCoords`,
        /// whose `z > 0` branch bakes to the OpenGL-style [-1,1] range; geometry
        /// placed through GESpace is therefore inside the clip volume on every
        /// backend GTE ships, which TE-baked depth is not. X and Y are identical
        /// to `translateCoords` for a viewport anchored at the origin.
        ///
        /// A degenerate viewport (zero width, height, or depth range) has no
        /// meaningful mapping; it logs an error and yields identity rather than
        /// propagating NaNs into a draw call.
        OMEGA_NODISCARD FMatrix<4,4> spaceToNDC() const;

        // -------------------------------------------------------------------
        // Objects and transforms
        // -------------------------------------------------------------------

        /// @brief Place a transform-only object in the space and return its
        /// handle. The object carries no geometry — it is a pure transform node
        /// (an anchor, or a placeholder to be given a mesh later).
        ///
        /// `addMesh()` is this plus a geometry reference.
        GESpaceObjectID addObject(const GESpaceTransform & transform = GESpaceTransform());

        /// @brief Place an existing GEMesh in the space and return its handle.
        ///
        /// The mesh is **referenced, never copied or re-baked**: its vertex
        /// buffer stays in its own local space and is shared with every other
        /// holder of that handle. Placing the same GEMesh twice is legitimate
        /// and is how you get two instances of one piece of geometry — they
        /// carry independent transforms and share one GPU buffer.
        ///
        /// This is the whole point of GESpace: `addMesh()` → `translate()` /
        /// `rotate()` / `scale()` → `objectTransform()` → draw. A mesh's own
        /// local extent (`GEMesh::bounds`) is what lets you pick those
        /// transform values without guessing a scale.
        ///
        /// A null mesh is rejected with an error log and returns
        /// `GESpaceInvalidObject`; every mutator on that handle then degrades
        /// loudly rather than moving a phantom.
        GESpaceObjectID addMesh(const SharedHandle<GEMesh> & mesh,
                                const GESpaceTransform & transform = GESpaceTransform());

        /// @brief The mesh placed at `id`, or null if the handle is unknown or
        /// names a transform-only object (`addObject`). Not an error either
        /// way — a transform node legitimately has no geometry.
        OMEGA_NODISCARD SharedHandle<GEMesh> meshOf(GESpaceObjectID id) const;

        /// @brief Remove an object from the space, dropping this space's
        /// reference to its mesh. The handle is retired, never recycled.
        /// Removing an unknown handle logs and is otherwise a no-op.
        void remove(GESpaceObjectID id);

        /// @brief Every live object handle, in the order the objects were
        /// added. Deterministic, so a renderer iterating this draws in a
        /// stable order frame to frame.
        OMEGA_NODISCARD OmegaCommon::Vector<GESpaceObjectID> objects() const;

        /// @brief Whether `id` names a live object in this space.
        OMEGA_NODISCARD bool contains(GESpaceObjectID id) const;

        /// @brief Replace the object's translation outright (space units).
        void setTranslation(GESpaceObjectID id, const GPoint3D & translation);
        /// @brief Move the object by a delta, in space units.
        void translate(GESpaceObjectID id, float dx, float dy, float dz);

        /// @brief Replace the object's orientation outright.
        void setRotation(GESpaceObjectID id, const FQuaternion & rotation);

        /// @brief Rotate the object by the given Euler angles (radians),
        /// applied X→Y→Z to match `rotationEuler` / `FQuaternion::fromEuler`.
        ///
        /// This COMPOSES with the current orientation rather than replacing it,
        /// about the space's axes: repeated calls accumulate, and because the
        /// composition is a quaternion product the orientation stays a pure
        /// rotation no matter how many times it is called.
        void rotate(GESpaceObjectID id, float pitch, float yaw, float roll);

        /// @brief Rotate the object by `radians` about the axis (ax, ay, az).
        /// Composes with the current orientation, like `rotate()`. The axis is
        /// normalized; a zero-length axis is a no-op.
        void rotateAxis(GESpaceObjectID id, float ax, float ay, float az, float radians);

        /// @brief Replace the object's scale outright.
        void setScale(GESpaceObjectID id, const GPoint3D & scale);
        /// @brief Multiply the object's current scale by these factors.
        void scale(GESpaceObjectID id, float sx, float sy, float sz);

        /// @brief The object's current TRS transform. An unknown handle logs an
        /// error and yields an identity transform.
        OMEGA_NODISCARD const GESpaceTransform & transformOf(GESpaceObjectID id) const;

        /// @brief The object's composed local→NDC matrix:
        /// `spaceToNDC()` applied after `transformOf(id).modelMatrix()`.
        ///
        /// **This is the final result** — hand it to the draw pipeline as the
        /// object's transform (Kreate consumes it as the per-object MVP; see
        /// Phase 5 of the plan). Geometry is never re-baked on the CPU: the
        /// mesh's vertex buffer stays in its own local space and this matrix
        /// does the work on the GPU.
        ///
        /// An unknown handle logs an error and yields `spaceToNDC()` alone
        /// (i.e. the object treated as untransformed), never a garbage matrix.
        OMEGA_NODISCARD FMatrix<4,4> objectTransform(GESpaceObjectID id) const;

        // Primitive placement (addPrimitive) arrives in Phase 4.

    private:
        struct Impl;
        UniqueHandle<Impl> impl;
    };

_NAMESPACE_END_

#endif
