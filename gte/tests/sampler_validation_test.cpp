/// Extension 8 §8.5 / §8.6 — backend-independent unit test for
/// `validateSamplerBindKind`, the runtime sampler-bind guard. Pure CPU, no
/// GPU device, so it runs on any host (including Metal-only builds).
///
/// This is the deterministic home for the §8.6 "negative" case. A real shader
/// cannot address a `static sampler` slot from a bind call — static samplers
/// take no `: N` register, so their layout location is not a stable bind
/// target — but the guard that rejects such a bind is this pure function, and
/// here we can feed it every layout-desc type directly.
///
/// The integer codes mirror `omegasl_shader_layout_desc_type` (omegasl.h); the
/// validator keys off these same magic numbers, pinned by the comments there.

#include <omegaGTE/GETexture.h>

#include <cassert>
#include <cstdio>

using namespace OmegaGTE;

namespace {
    enum LayoutDescType {
        CONSTANT            = 0,
        BUFFER              = 1,
        TEXTURE1D           = 2,
        TEXTURE2D           = 3,
        TEXTURE3D           = 4,
        SAMPLER1D           = 5,
        SAMPLER2D           = 6,
        SAMPLER3D           = 7,
        STATIC_SAMPLER1D    = 8,
        STATIC_SAMPLER2D    = 9,
        STATIC_SAMPLER3D    = 10,
        SAMPLERCUBE         = 17,
        STATIC_SAMPLERCUBE  = 18,
        UNIFORM             = 19,
    };
}

int main() {
    // Accept: every runtime (non-static) sampler type resolves cleanly.
    assert(validateSamplerBindKind(SAMPLER1D,   "vs", 0));
    assert(validateSamplerBindKind(SAMPLER2D,   "vs", 1));
    assert(validateSamplerBindKind(SAMPLER3D,   "vs", 2));
    assert(validateSamplerBindKind(SAMPLERCUBE, "vs", 3));

    // Reject: a slot the shader declared `static`. The validator emits a
    // diagnostic to stderr (expected here) and returns false so the bind is
    // skipped — and the bind methods assert on this in debug builds.
    assert(!validateSamplerBindKind(STATIC_SAMPLER1D,   "vs", 0));
    assert(!validateSamplerBindKind(STATIC_SAMPLER2D,   "vs", 1));
    assert(!validateSamplerBindKind(STATIC_SAMPLER3D,   "vs", 2));
    assert(!validateSamplerBindKind(STATIC_SAMPLERCUBE, "vs", 3));

    // Reject: a non-sampler slot (binding a sampler to a buffer/texture/uniform
    // register is a programmer error).
    assert(!validateSamplerBindKind(CONSTANT,  "vs", 0));
    assert(!validateSamplerBindKind(BUFFER,    "vs", 1));
    assert(!validateSamplerBindKind(TEXTURE1D, "vs", 2));
    assert(!validateSamplerBindKind(TEXTURE2D, "vs", 3));
    assert(!validateSamplerBindKind(TEXTURE3D, "vs", 4));
    assert(!validateSamplerBindKind(UNIFORM,   "vs", 5));

    std::printf("sampler validation test passed\n");
    return 0;
}
