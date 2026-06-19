#ifndef OMEGASL_SHADERARCHIVE_H
#define OMEGASL_SHADERARCHIVE_H

#include <omegasl.h>

#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

namespace omegasl {

/// In-memory mirror of a `.omegasllib` archive, decoupled from any GTE device.
///
/// This is the single (de)serializer for the on-disk container format — the
/// compiler's writer (`CodeGen::linkShaderObjects`), the engine's reader
/// (`OmegaGraphicsEngine::loadShaderLibraryFromInputStream`), and the link
/// tool all go through `ReadShaderArchive` / `WriteShaderArchive` instead of
/// hand-rolling the byte layout three times (the drift hazard from
/// OmegaSL-Linker-And-Headers-Plan §1.3.3).
///
/// The archive **owns all backing storage** the `shaders` records point at.
/// Each name / bytecode blob / layout array / vertex-param array is a separate
/// heap allocation, so the owning vectors can grow while the raw pointers held
/// in `shaders` (and copied into `GTEShader::internal` at load) stay valid for
/// the archive's lifetime. `ReadShaderArchive` populates every buffer; the
/// writer path only needs `internData` (bytecode read from object files) — the
/// record metadata aliases compiler-owned memory that outlives the write call.
struct OmegaSLShaderArchive {
    std::string name;
    std::uint8_t backendId = 0;
    std::uint32_t formatVersion = OMEGASLLIB_FORMAT_VERSION;
    std::vector<omegasl_shader> shaders;

    /// Copy `len` bytes of `src` (a shader / vertex-param name) plus a NUL
    /// terminator into owned storage and return a stable pointer.
    char *internName(const char *src, std::size_t len);
    /// Copy `len` bytes of bytecode into owned storage and return a stable
    /// pointer. `len == 0` returns `nullptr` (the stub / no-body convention).
    unsigned char *internData(const void *src, std::size_t len);
    /// Allocate a zero-initialized layout-descriptor array of `count`
    /// (`nullptr` if `count == 0`) and return a stable pointer.
    omegasl_shader_layout_desc *allocLayout(std::size_t count);
    /// Allocate a zero-initialized vertex-param array of `count`
    /// (`nullptr` if `count == 0`) and return a stable pointer.
    omegasl_vertex_shader_param_desc *allocParams(std::size_t count);

private:
    std::vector<std::unique_ptr<char[]>> nameStore_;
    std::vector<std::unique_ptr<unsigned char[]>> dataStore_;
    std::vector<std::unique_ptr<omegasl_shader_layout_desc[]>> layoutStore_;
    std::vector<std::unique_ptr<omegasl_vertex_shader_param_desc[]>> paramStore_;
};

/// Parse a `.omegasllib` byte stream into `out` (fully owned, no GTE device,
/// no feature gating, no `_loadShaderFromDesc`). Validates the container prefix
/// (magic + format version) and bounds every length against defensive caps so
/// a malformed or hostile file fails loudly rather than allocating wildly.
/// Returns false and fills `err` on any malformed input or stream failure.
bool ReadShaderArchive(std::istream &in, OmegaSLShaderArchive &out, std::string &err);

/// Serialize `lib` to `out` in the on-disk container format (prefix + body).
/// A record whose `dataSize == 0` is written as a header-only stub (no
/// bytecode, no stage decoration), matching the reader. Returns false and
/// fills `err` on a stream failure.
bool WriteShaderArchive(std::ostream &out, const OmegaSLShaderArchive &lib, std::string &err);

} // namespace omegasl

#endif // OMEGASL_SHADERARCHIVE_H
