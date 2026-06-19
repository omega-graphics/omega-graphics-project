#ifndef OMEGASL_PREPROCESSOR_H
#define OMEGASL_PREPROCESSOR_H

#include <cstdint>
#include <map>
#include <string>

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

    /// Reject `#include` directives during processing. The runtime
    /// `OmegaSLCompiler` flips this on because a runtime source string
    /// is inline content with no file-system context for resolving
    /// include paths — loud rejection prevents silent drops. When set
    /// and an `#include` is seen, the directive is skipped, a precise
    /// stderr error fires, and `hasErrors()` flips true. Offline path
    /// leaves this off and `#include` resolves against `currentPath`
    /// exactly as before.
    void setRejectIncludes(bool reject) { rejectIncludes_ = reject; }

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
    uint64_t requiredFeatures_ = 0;
    uint64_t unsatisfiedFeatures_ = 0;
    bool backendSet_ = false;
    bool rejectIncludes_ = false;
    bool hasErrors_ = false;

    std::string processInternal(const std::string& source, const std::string& currentPath, unsigned includeDepth);
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
