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

        /// Map of OmegaSL resource register → texture. The triangulation
        /// builder seeds this with the attachment's texture when one is
        /// supplied; callers may add or replace entries before drawing.
        std::map<unsigned, SharedHandle<GETexture>> textureBindings;

        ~GEMesh() = default;
    };

    /// @brief Compute the byte stride implied by an attribute bitmask.
    OMEGAGTE_EXPORT size_t geMeshStrideFor(uint32_t attributes);

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
