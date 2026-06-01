#ifndef KREATE_MESH_H
#define KREATE_MESH_H

#include "Base.h"
#include <cstddef>
#include <cstdint>
#include <memory>

namespace Kreate {

/// Bitmask flags describing the attributes packed into a kREATE mesh's
/// vertex buffer. Attributes appear in this fixed order with no
/// padding:
///
///   Position (float3, 12B) -> UV2 (float2, 8B) -> UV3 (float3, 12B)
///                          -> Normal (float3, 12B) -> Color (float4, 16B).
///
/// The OmegaSL `buffer<T>` struct the shader declares must use the
/// same order and types for every attribute that is enabled. This
/// mirrors `OmegaGTE::GEMeshVertexAttribute` so a kREATE mesh maps
/// to the underlying GTE handle without a translation step.
namespace VertexAttribute {
    constexpr std::uint32_t Position = 1u << 0;
    constexpr std::uint32_t UV2      = 1u << 1;
    constexpr std::uint32_t UV3      = 1u << 2;
    constexpr std::uint32_t Normal   = 1u << 3;
    constexpr std::uint32_t Color    = 1u << 4;
} // namespace VertexAttribute

/// Primitive topology for a mesh.
enum class MeshTopology : std::uint8_t {
    Triangle,
    TriangleStrip,
};

/// Index element type. `None` means the mesh has no index buffer; the
/// draw call resolves to a non-indexed draw.
enum class IndexFormat : std::uint8_t {
    None,
    UInt16,
    UInt32,
};

/// Describes the vertex layout and topology of a `Mesh`.
struct KREATE_EXPORT MeshDesc {
    /// Bitwise OR of `VertexAttribute` flags. Position is required.
    std::uint32_t attributes  = VertexAttribute::Position;
    MeshTopology  topology    = MeshTopology::Triangle;
    IndexFormat   indexFormat = IndexFormat::None;
};

/// Byte stride of one vertex implied by `attributes`.
KREATE_EXPORT std::size_t vertexStrideFor(std::uint32_t attributes);

/// Opaque mesh handle. Constructed only via `App::createMesh`.
class KREATE_EXPORT Mesh {
public:
    ~Mesh();

    Mesh(const Mesh &) = delete;
    Mesh &operator=(const Mesh &) = delete;

private:
    Mesh();
    struct Impl;
    std::unique_ptr<Impl> impl;

    friend struct MeshFactory;
};

} // namespace Kreate

#endif // KREATE_MESH_H
