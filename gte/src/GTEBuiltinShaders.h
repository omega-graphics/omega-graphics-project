#include "omegaGTE/GTEBase.h"
#include "omegasl.h"

#ifndef OMEGAGTE_GTEBUILTINSHADERS_H
#define OMEGAGTE_GTEBUILTINSHADERS_H

_NAMESPACE_BEGIN_

/// @brief Engine-independent access to `GTEBuiltinShaders.omegasllib` -- the
/// merged, build-time-compiled library of GTE's internal-use OmegaSL kernels
/// (Triangulation Engine GPU kernels, mipmap generation, blit pipeline
/// shaders; see `gte/src/shaders/`). Embedded into OmegaGTE as a byte array
/// (Triangulation-Engine-Completion-Plan.md Phase 4.2 -- no runtime
/// filesystem dependency), so this is reachable from call sites that have no
/// `OmegaGraphicsEngine` handle to call `loadShaderLibrary` on -- notably
/// `OmegaTriangulationEngineContext`, which only ever sees a raw native
/// device/queue obtained from a render target. Parses via
/// `omegasl::ReadShaderArchive`, the same engine-independent (de)serializer
/// `OmegaGraphicsEngine::loadShaderLibraryFromInputStream` uses internally
/// (see OmegaSL-Linker-And-Headers-Plan.md).
namespace GTEBuiltinShaders {
    // Triangulation Engine GPU kernels (Phase 4.1 -- ported from the
    // per-target HLSL/Metal/SPIR-V that used to live inline in
    // D3D12TEContext.cpp / MetalTEContext.mm / VulkanTessSpirv.inc).
    constexpr const char *TriangulateRect      = "triangulate_rect_kernel";
    constexpr const char *TriangulateEllipsoid = "triangulate_ellipsoid_kernel";
    constexpr const char *TriangulateRectPrism = "triangulate_rect_prism_kernel";
    constexpr const char *TriangulatePath2D    = "triangulate_path2d_kernel";

    /// @brief Look up a compiled shader by its OmegaSL entry-point name.
    /// @returns A pointer valid for the process lifetime, or nullptr if the
    /// embedded library failed to parse or no shader has that name.
    const omegasl_shader *find(const char *name);
}

_NAMESPACE_END_

#endif
