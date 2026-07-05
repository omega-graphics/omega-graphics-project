#include "GTEBase.h"

#ifndef OMEGAGTE_GEBLITSHADERS_H
#define OMEGAGTE_GEBLITSHADERS_H

_NAMESPACE_BEGIN_

/// @brief Entry-point names of the engine's built-in blit pipeline shaders
/// (Pipeline-Completion-Extension-Plan.md, Extension 3 / §3.4).
///
/// Sources live under `gte/src/shaders/` (`blit_fullscreen_vs.omegasl`,
/// `blit_copy.omegasl`) and share the `OmegaGTEBlitVertexData` rasterizer
/// contract declared in `gte/src/shaders/BlitShaders.omegaslh`. They are
/// compiled once into `GTEBuiltinShaders.omegasllib`, the same merged
/// library the Triangulation Engine's GPU kernels ship in.
///
/// `blitWithPipeline()` always runs `FullscreenVertex` ahead of the
/// caller-supplied fragment shader in a `BlitPipelineDescriptor` -- callers
/// never bind it directly. `Copy` is a ready-to-use fragment shader for the
/// common case of a straight passthrough sample (hardware-copy-equivalent,
/// but through the programmable pipeline so format conversion still works).
///
/// Additional built-ins (`blit_linear`, `blit_srgb_encode`,
/// `blit_srgb_decode`, `blit_tonemap_reinhard`) remain deferred per
/// Pipeline-Completion-Extension-Plan.md §3.4 "Deferred for follow-up".
namespace GEBlitShaderNames {
    /// Full-screen-triangle vertex stage (blit_fullscreen_vs.omegasl).
    constexpr const char *FullscreenVertex = "omega_gte_blit_fullscreen_vs";
    /// Passthrough source-texture sample (blit_copy.omegasl).
    constexpr const char *Copy = "blit_copy";
}

_NAMESPACE_END_

#endif
