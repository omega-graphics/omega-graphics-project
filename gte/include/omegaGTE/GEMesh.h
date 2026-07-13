#include "GTEBase.h"
#include "TE.h"

#include <map>

#ifndef OMEGAGTE_GEMESH_H
#define OMEGAGTE_GEMESH_H

_NAMESPACE_BEGIN_
// Guard against: X11 None = 0L
#ifdef None 
#undef None
#endif

    class GEBuffer;
    class GETexture;
    class OmegaGraphicsEngine;

    /// @brief Per-vertex attribute flags describing the contents of a GEMesh's
    /// vertex buffer. Stored in `GEMeshDescriptor::attributes` as a bitmask.
    /// Attributes are written in this fixed order with no padding:
    ///   Position (float3, 12B) → UV2 (float2, 8B) → UV3 (float3, 12B)
    ///   → Normal (float3, 12B) → Color (float4, 16B).
    /// The shader-side `buffer<T>` struct must match this order and these
    /// types for the attributes that are present.
    enum GEMeshVertexAttribute : uint32_t {
        GEMeshAttrPosition = 1u << 0,
        GEMeshAttrUV2      = 1u << 1,
        GEMeshAttrUV3      = 1u << 2,
        GEMeshAttrNormal   = 1u << 3,
        GEMeshAttrColor    = 1u << 4,
    };

    /// @brief Primitive topology for a GEMesh. Maps to
    /// `GECommandBuffer::PolygonType` at draw time.
    enum class GEMeshTopology : uint8_t {
        Triangle,
        TriangleStrip,
    };

    /// @brief Index element type. `None` means the mesh has no index buffer.
    /// `buildMeshFromTriangulation` emits `UInt32` when the source TEMesh
    /// has been deduplicated via `TEMesh::buildIndexed()` (see
    /// Triangulation-Engine-Completion-Plan Phase 3); `UInt16` is not yet
    /// supported there (no 16-bit `GEBufferWriter` path). MeshAsset loaders
    /// still only emit `None`.
    enum class GEMeshIndexType : uint8_t {
        None,
        UInt16,
        UInt32,
    };

    /// @brief Describes the layout and topology of a GEMesh.
    struct OMEGAGTE_EXPORT GEMeshDescriptor {
        /// Bitwise OR of GEMeshVertexAttribute. Position is required.
        uint32_t attributes = GEMeshAttrPosition;
        GEMeshTopology topology = GEMeshTopology::Triangle;
        GEMeshIndexType indexType = GEMeshIndexType::None;
    };

    /// @brief The axis-aligned bounding box of a mesh's vertices, in the
    /// mesh's own **local space** — the units the source file was authored
    /// in, never NDC.
    ///
    /// A mesh's vertex buffer is a GPU resource and the CPU copy is dropped
    /// after upload, so without this a caller cannot answer "how big is this
    /// thing?" — and cannot place it in a scene without guessing a scale.
    /// Loaders fill it in while they still hold the vertex stream, which
    /// costs one pass over data they are already walking.
    struct OMEGAGTE_EXPORT GEMeshBounds {
        GPoint3D min {0.f, 0.f, 0.f};
        GPoint3D max {0.f, 0.f, 0.f};

        /// False until a loader populates the box. An empty mesh (no
        /// vertices) has no meaningful bounds and leaves this false rather
        /// than reporting a degenerate box at the origin, which a caller
        /// would happily divide by.
        bool valid = false;

        /// @brief Midpoint of the box. The point to translate to the origin
        /// to center the mesh on its own bounds.
        OMEGA_NODISCARD GPoint3D center() const;
        /// @brief Per-axis size (max - min).
        OMEGA_NODISCARD GPoint3D extent() const;
        /// @brief The largest of the three extents — the scalar to divide by
        /// when fitting a mesh into a box of known size.
        OMEGA_NODISCARD float longestExtent() const;
    };

    /// @brief Compute the local-space AABB over the Position attribute of a
    /// packed, non-indexed vertex stream.
    ///
    /// Position is always the first three floats of a vertex (the attribute
    /// order in `GEMeshVertexAttribute` is fixed and Position is required),
    /// so this reads `packed[i * stride/4 .. +2]` and ignores the rest.
    /// @param packed      Interleaved vertex floats, exactly as uploaded.
    /// @param vertexCount Number of vertices in `packed`.
    /// @param vertexStride Per-vertex stride in **bytes**.
    /// @returns A populated box, or `valid == false` if the inputs describe
    ///          no vertices or a stride too small to hold a position.
    OMEGAGTE_EXPORT GEMeshBounds geMeshComputeBounds(const float *packed,
                                                     unsigned vertexCount,
                                                     size_t vertexStride);

    /// @brief A GPU-ready mesh: vertex buffer (and optional index buffer),
    /// layout descriptor, and a slot-keyed table of texture bindings.
    /// Backend-agnostic — backends consume the underlying GEBuffer / GETexture
    /// handles directly.
    class OMEGAGTE_EXPORT GEMesh {
    public:
        OMEGACOMMON_CLASS("OmegaGTE.GEMesh")

        SharedHandle<GEBuffer> vertexBuffer;
        /// Null when descriptor.indexType == None.
        SharedHandle<GEBuffer> indexBuffer;

        unsigned vertexCount = 0;
        /// Only meaningful when indexBuffer is non-null.
        unsigned indexCount = 0;
        /// Per-vertex stride in bytes, derived from `descriptor.attributes`.
        size_t vertexStride = 0;

        GEMeshDescriptor descriptor;

        /// Local-space AABB of `vertexBuffer`, filled in at load/build time.
        /// `bounds.valid` is false on a mesh built by a path that does not
        /// populate it (or on an empty mesh) — check before dividing by an
        /// extent. This is what lets a caller scale a mesh to fit a viewport
        /// (see `GESpace`) instead of hand-tuning a magic scale per asset.
        GEMeshBounds bounds;

        /// Map of OmegaSL resource register → texture. The triangulation
        /// builder seeds this with the attachment's texture when one is
        /// supplied; callers may add or replace entries before drawing.
        std::map<unsigned, SharedHandle<GETexture>> textureBindings;

        ~GEMesh() = default;
    };

    /// @brief The byte stride of one vertex **as the GPU reads it** — the
    /// stride to allocate, upload, and set as `GEMesh::vertexStride`.
    ///
    /// This is NOT the sum of the attributes' component sizes. A `buffer<T>` is
    /// laid out by the backend's buffer standard, and under std430 (Vulkan) a
    /// `float3` has a **16-byte base alignment**, so a Position-only vertex
    /// occupies 16 bytes, not 12 — the compiled SPIR-V decorates the array with
    /// `ArrayStride = 16`. D3D12's `StructuredBuffer` uses scalar layout and
    /// really is 12; Metal's `simd_float3` is 16. The number differs per
    /// backend, which is exactly why this defers to `omegaSLStructStride`
    /// rather than hand-summing sizes (it used to hand-sum, and every
    /// MeshAsset-loaded mesh on Vulkan/Metal was read at the wrong stride — the
    /// vertices drifted and the geometry came out shredded).
    OMEGAGTE_EXPORT size_t geMeshStrideFor(uint32_t attributes);

    /// @brief The byte stride of one vertex **tightly packed**, with each
    /// attribute's components back to back and no padding (Position = 12B).
    ///
    /// This is an authoring-side layout, not a GPU one: it is what a loader
    /// naturally produces while walking a source file. Convert it with
    /// `geMeshRepackToGPULayout` before upload. Never hand it to the GPU.
    OMEGAGTE_EXPORT size_t geMeshTightStrideFor(uint32_t attributes);

    /// @brief Re-lay a tightly-packed vertex stream into the GPU's buffer
    /// layout, inserting whatever padding the backend's standard requires.
    ///
    /// Each attribute is moved to the offset `omegaSLStructMemberOffsets`
    /// reports, and each vertex is placed `geMeshStrideFor` bytes apart. Pad
    /// bytes are zeroed. When the two layouts already agree (D3D12's scalar
    /// `StructuredBuffer`), this is a straight copy.
    /// @param tightPacked Vertex floats at `geMeshTightStrideFor` spacing.
    /// @param attributes  The layout both strides are derived from.
    /// @param vertexCount Number of vertices in `tightPacked`.
    OMEGAGTE_EXPORT OmegaCommon::Vector<float> geMeshRepackToGPULayout(
        const OmegaCommon::Vector<float> & tightPacked,
        uint32_t attributes,
        unsigned vertexCount);

    /// @brief Build a GEMesh from a TETriangulationResult.
    /// When `desc.indexType == None`, flattens `vertexPolygons` into a
    /// single contiguous, non-indexed vertex buffer (one entry per triangle
    /// corner; shared vertices duplicated). When `desc.indexType ==
    /// UInt32`, builds the vertex buffer from the deduplicated
    /// `result.mesh.indexedData` instead — the caller must have already
    /// called `TEMesh::buildIndexed()` on the source mesh, or this returns
    /// null with an error log. `UInt16` is not yet supported (no 16-bit
    /// `GEBufferWriter` path). Attributes that are requested but not present
    /// on a vertex's AttachmentData are written as zero (warning logged once).
    /// @param engine Graphics engine used to allocate the vertex (and, for
    ///               indexed output, index) buffer.
    /// @param result Triangulation output to flatten.
    /// @param desc Vertex layout, topology, and index-type intent. Only
    ///             `topology == Triangle` is supported; other values produce
    ///             a null return and an error log.
    /// @param diffuseTexture Optional texture to seed `textureBindings`
    ///                       with. Pass the same SharedHandle that lives
    ///                       on the source `TETriangulationParams::Attachment`.
    /// @param diffuseSlot OmegaSL resource register for that texture.
    /// @returns A populated GEMesh, or null on error.
    OMEGAGTE_EXPORT SharedHandle<GEMesh> buildMeshFromTriangulation(
        OmegaGraphicsEngine *engine,
        const TETriangulationResult &result,
        const GEMeshDescriptor &desc,
        SharedHandle<GETexture> diffuseTexture = nullptr,
        unsigned diffuseSlot = 0);

_NAMESPACE_END_

#endif
