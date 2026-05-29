#ifndef OMEGAGTE_COMMON_MESHPARSER_H
#define OMEGAGTE_COMMON_MESHPARSER_H

#include "omegaGTE/GEMesh.h"

#include <string>
#include <vector>

_NAMESPACE_BEGIN_

// Backend-neutral mesh parser. Lives under src/common/ so the D3D12, Vulkan,
// and Metal asset backends can share format dispatch and CPU-side packing.
// Metal goes through Model I/O for the formats it supports natively (OBJ,
// glTF) and only routes .fbx here, since Model I/O cannot load FBX.
//
// Supported formats: glTF 2.0 (.gltf / .glb) via cgltf, Wavefront OBJ via an
// inline parser, and FBX (.fbx) via ufbx. Vertices are emitted in the source
// file's raw coordinate space / units — no axis or unit conversion — so a
// model loads identically regardless of format.
namespace MeshParser {

struct ParsedMesh {
    /// Packed per-vertex stream matching the requested
    /// `GEMeshDescriptor` attribute order:
    ///   Position → UV2 → UV3 → Normal → Color.
    /// Missing attributes are written as zeros.
    std::vector<float> packed;
    /// Number of vertices in `packed` (= packed.size()*4 / stride).
    unsigned vertexCount = 0;
    /// Stride in bytes implied by `desc.attributes`.
    size_t stride = 0;
    /// First base-color texture path encountered while walking the
    /// asset, resolved relative to the source file. Empty if none.
    std::string baseColorTexturePath;
};

/// Parse the file at `path` into a flat non-indexed triangle stream
/// matching `desc`. Topology is forced to Triangle; indexType must be
/// None (matching the Phase 3 v1 contract). Returns false and logs to
/// stderr on failure.
bool parseMesh(const std::string &path,
               const GEMeshDescriptor &desc,
               ParsedMesh &out);

}  // namespace MeshParser

_NAMESPACE_END_

#endif
