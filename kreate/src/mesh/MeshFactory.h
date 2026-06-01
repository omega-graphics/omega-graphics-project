#ifndef KREATE_INTERNAL_MESH_FACTORY_H
#define KREATE_INTERNAL_MESH_FACTORY_H

#include <kreate/Mesh.h>
#include <omegaGTE/GTEDevice.h>
#include <omegaGTE/GEMesh.h>
#include <cstddef>
#include <memory>

namespace Kreate {

/// Internal entry points for the mesh subsystem. `App::createMesh` routes
/// here, handing in the GTE borrowed from the renderer (`Renderer::gte()`).
/// Kept out of the public KREATE headers — building a mesh needs OmegaGTE
/// types that the public surface deliberately hides. Mirrors the shape of
/// `PipelineFactory`.
struct MeshFactory {
    /// Allocate an Upload-storage GEBuffer for the vertex (and optional
    /// index) data, `memcpy` the caller's bytes in, and wrap the
    /// resulting GEMesh in a `Kreate::Mesh`. Returns nullptr on
    /// validation failure (Position missing, byte/count mismatch,
    /// indexFormat / indexData inconsistency, etc.) with a diagnostic
    /// logged to `std::cerr`.
    static std::shared_ptr<Mesh> create(OmegaGTE::GTE &gte,
                                        const MeshDesc &desc,
                                        const void *vertexData,
                                        std::size_t vertexBytes,
                                        unsigned vertexCount,
                                        const void *indexData,
                                        std::size_t indexBytes,
                                        unsigned indexCount);

    /// Internal accessor for the renderer — returns the GEMesh wrapped by
    /// `m`. Lives on the factory so `Mesh::Impl` does not have to be
    /// re-exposed in a separate header.
    static SharedHandle<OmegaGTE::GEMesh> &geMesh(Mesh &m);
};

} // namespace Kreate

#endif // KREATE_INTERNAL_MESH_FACTORY_H
