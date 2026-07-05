#ifndef OMEGASL_PREPROCESSOR_H
#define OMEGASL_PREPROCESSOR_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace omegasl {

/// Active codegen backend, used by the Preprocessor to decide which
/// `OMEGASL_BACKEND_<X>` and `OMEGASL_FEATURE_<NAME>` macros to predefine.
/// The macro set encodes whether a feature is *expressible* in the
/// generated source for that backend at its highest capability — not
/// whether the user's hardware supports it. Hardware feasibility is
/// gated at runtime (see Layer 3, OmegaSL-Feature-Gap-Survey §14.3).
enum class PPBackend {
    HLSL,
    MSL,
    GLSL
};

class Preprocessor {
public:
    static const unsigned kMaxIncludeDepth = 10;

    /// Predefine `OMEGASL_BACKEND_<HLSL|MSL|GLSL>` and the static set of
    /// `OMEGASL_FEATURE_<NAME>` macros for `backend`. Should be called
    /// once before `process` so source-level `#if defined(...)` and
    /// `#requires(...)` resolve against the active backend.
    void setBackend(PPBackend backend);

    void define(const std::string& name, const std::string& value = "1");
    bool isDefined(const std::string& name) const;
    std::string process(const std::string& source, const std::string& currentPath = "");

    /// Append a directory to the `#include` search path. A quoted
    /// `#include "foo.omegaslh"` is resolved against the including file's
    /// own directory first (the `currentPath` handed to `process`), then
    /// against each directory added here, in the order they were added —
    /// matching a C preprocessor's `"..."` search order. The compiler
    /// driver (`omegaslc`) populates these from its `-I` / `--include-dir`
    /// options so a project can include shared headers that do not sit
    /// next to the translation unit. Has no effect while `rejectIncludes_`
    /// is set (the runtime path forbids `#include` outright).
    void addIncludeDir(const std::string& dir);

    /// Reject `#include` directives during processing. The runtime
    /// `OmegaSLCompiler` flips this on because a runtime source string
    /// is inline content with no file-system context for resolving
    /// include paths — loud rejection prevents silent drops. When set
    /// and an `#include` is seen, the directive is skipped, a precise
    /// stderr error fires, and `hasErrors()` flips true. Offline path
    /// leaves this off and `#include` resolves against `currentPath`
    /// exactly as before.
    void setRejectIncludes(bool reject) { rejectIncludes_ = reject; }

    /// Emit one output line for every input line, so output line numbers
    /// match the source 1:1. Normally the preprocessor drops directive lines
    /// (`#define`, `#ifdef`, ...) and `#if`-skipped regions, which shifts the
    /// line numbers of everything below them. Tooling that maps compiler
    /// diagnostics back onto the *original* buffer — the language server
    /// (`omegasl-lsp`) — needs the numbers to stay aligned, so it turns this
    /// on. Consumed/skipped lines are replaced by an empty line instead of
    /// being removed. On its own this assumes `#include` expansion is disabled
    /// (`setRejectIncludes(true)`): an expanded include emits more than one
    /// line and would break the 1:1 mapping — UNLESS a source map is also being
    /// built (`setSourceMap(true)`), which records where each output line came
    /// from and lifts that restriction. Default (false) leaves the `omegaslc`
    /// output byte-identical.
    void setLinePreserving(bool preserve) { linePreserving_ = preserve; }

    /// Build a source-line map alongside the processed output, so a consumer
    /// can translate an output-line number back to the editor buffer even when
    /// `#include`s expand inline (which shifts everything below them). With
    /// this on, `#include` may be enabled (`setRejectIncludes(false)`) while
    /// line-preservation still holds for the *top-level* buffer: the map
    /// records, for every output line, the 1-based top-level source line that
    /// produced it — or 0 when the line came from included (foreign) content.
    /// The language server uses this to keep diagnostics/symbols aligned with
    /// the buffer and to drop header-internal diagnostics. Off by default, so
    /// `omegaslc` is unaffected.
    void setSourceMap(bool enable) { buildSourceMap_ = enable; }

    /// The source-line map from the most-recent `process()` when
    /// `setSourceMap(true)` was set: `sourceMap()[i]` is the 1-based top-level
    /// source line of output line `i+1`, or 0 for included content. Empty when
    /// source-map mode is off.
    const std::vector<unsigned> & sourceMap() const { return sourceMap_; }

    /// True if any directive in the most-recently-processed source
    /// failed in a way that should abort downstream compilation
    /// (today: `#include` while `rejectIncludes_` is true). Sticky
    /// across the lifetime of this Preprocessor instance — construct
    /// a fresh one per compile if isolation is needed.
    bool hasErrors() const { return hasErrors_; }

    /// Union of every `#requires(...)` feature declared at file scope in
    /// the most-recently-processed source. Each bit is one of the
    /// `OMEGASL_FEATURE_BIT_*` values from `omegasl.h`. Codegen propagates
    /// this onto every `omegasl_shader` record produced from the file.
    uint64_t requiredFeatures() const { return requiredFeatures_; }

    /// Subset of `requiredFeatures()` whose corresponding
    /// `OMEGASL_FEATURE_<NAME>` macro is *not* defined for the active
    /// backend. A non-zero value means at least one declared requirement
    /// cannot be expressed in the generated source — codegen emits a
    /// header-only "stub" for every shader in the file (see §14.1, user-
    /// requested twist: no hard fail, runtime decides via the bitfield).
    uint64_t unsatisfiedRequiredFeatures() const { return unsatisfiedFeatures_; }

private:
    std::map<std::string, std::string> macros_;
    /// Extra `#include` search directories, in priority order (after the
    /// including file's own directory). Populated by `addIncludeDir`.
    std::vector<std::string> includeDirs_;
    uint64_t requiredFeatures_ = 0;
    uint64_t unsatisfiedFeatures_ = 0;
    bool backendSet_ = false;
    bool rejectIncludes_ = false;
    bool linePreserving_ = false;
    bool buildSourceMap_ = false;
    bool hasErrors_ = false;
    /// One entry per output line of the most-recent `process()` (top-level
    /// only): the 1-based source line that produced it, or 0 for included
    /// content. Populated only when `buildSourceMap_` is set.
    std::vector<unsigned> sourceMap_;

    std::string processInternal(const std::string& source, const std::string& currentPath, unsigned includeDepth);
    /// Resolve a quoted `#include "incPath"` to an openable file path, or
    /// the empty string if no candidate exists. Search order mirrors a C
    /// preprocessor's `"..."` form: the including file's directory
    /// (`currentPath`) first, then each directory from `addIncludeDir` in
    /// order. When `currentPath` is empty (a top-level source with no
    /// directory component) the bare `incPath` is tried before the search
    /// dirs, preserving the original relative-to-CWD behavior.
    std::string resolveIncludePath(const std::string& currentPath, const std::string& incPath) const;
    std::string expandMacros(const std::string& line) const;
    /// Parse a single argument list of the form `(NAME[, NAME...])` or
    /// `NAME` (bare). On success, OR each named feature's bit into
    /// `requiredFeatures_`; if the corresponding `OMEGASL_FEATURE_<NAME>`
    /// macro is not currently defined, also OR it into
    /// `unsatisfiedFeatures_`. Unknown names are diagnosed to stderr.
    void handleRequiresDirective(const std::string& argText);
    /// Evaluate a single `defined(NAME)` / `defined NAME` expression.
    /// Returns true if `NAME` is a currently defined macro. The
    /// expression form supports bare identifiers as a degenerate case
    /// (`#if FOO` is interpreted as `#if defined(FOO)`).
    bool evaluateIfExpression(const std::string& expr) const;
    /// Scan already-preprocessed `#include`d content for a shader entry
    /// point. A header (`.omegaslh`) is textually inlined into every
    /// translation unit that includes it, so a shader declared in one
    /// would be compiled into — and collide across — each including unit.
    /// Returns true (and fills `keywordOut` / `lineOut`) if a stage
    /// keyword (`vertex` / `fragment` / `compute` / `hull` / `domain` /
    /// `mesh`) appears as a real token. Drives the existing `Lexer` so
    /// the same keyword spelled inside a comment, a string literal, or as
    /// a substring of an identifier (`fragment_like_helper`) does *not*
    /// trip it — single source of truth for "what is a stage keyword".
    bool includeDeclaresShader(const std::string& processedContent,
                               std::string& keywordOut, unsigned& lineOut) const;
};

} // namespace omegasl

#endif
