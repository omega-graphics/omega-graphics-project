#include "Preprocessor.h"
#include "Lexer.h"

#include <omegasl.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>

namespace omegasl {

namespace {

/// Single source of truth for the `OMEGASL_FEATURE_<NAME>` macro set,
/// the matching `OMEGASL_FEATURE_BIT_*` value, and whether each backend
/// can express the feature *at its highest capability* (the user-
/// requested rule for Layer 1: assume the platform can express RT,
/// mesh shaders, etc.; mark features genuinely impossible for a backend
/// (e.g. `DOUBLE` on MSL) as not expressible — those still tag the
/// shader so runtime rejection sees them, but the body transpiles as
/// null). Maps directly to the table in OmegaSL-Feature-Gap-Survey §14.5.
struct FeatureEntry {
    const char* name;          ///< Suffix; macro spelling is OMEGASL_FEATURE_<name>.
    uint64_t bit;              ///< Matching OMEGASL_FEATURE_BIT_* value.
    bool hlslExpressible;
    bool mslExpressible;
    bool glslExpressible;
};

constexpr std::array<FeatureEntry, 16> kFeatureTable = {{
    {"RAYTRACING",            OMEGASL_FEATURE_BIT_RAYTRACING,            true,  true,  true},
    {"MESH_SHADERS",          OMEGASL_FEATURE_BIT_MESH_SHADERS,          true,  true,  true},
    {"GEOMETRY_SHADERS",      OMEGASL_FEATURE_BIT_GEOMETRY_SHADERS,      true,  false, true},
    {"TESSELLATION",          OMEGASL_FEATURE_BIT_TESSELLATION,          true,  false, true},
    {"SUBGROUP_OPS",          OMEGASL_FEATURE_BIT_SUBGROUP_OPS,          true,  true,  true},
    {"BINDLESS",              OMEGASL_FEATURE_BIT_BINDLESS,              true,  true,  true},
    {"FLOAT16",               OMEGASL_FEATURE_BIT_FLOAT16,               true,  true,  true},
    {"INT64",                 OMEGASL_FEATURE_BIT_INT64,                 true,  true,  true},
    {"VARIABLE_RATE",         OMEGASL_FEATURE_BIT_VARIABLE_RATE,         true,  true,  true},
    {"SUBPASS_INPUTS",        OMEGASL_FEATURE_BIT_SUBPASS_INPUTS,        false, false, true},
    {"SPEC_CONSTANTS",        OMEGASL_FEATURE_BIT_SPEC_CONSTANTS,        true,  true,  true},
    {"TEXTURECUBE_RW",        OMEGASL_FEATURE_BIT_TEXTURECUBE_RW,        true,  false, false},
    {"TEXTURE2D_MS_WRITE",    OMEGASL_FEATURE_BIT_TEXTURE2D_MS_WRITE,    false, false, false},
    {"DOUBLE",                OMEGASL_FEATURE_BIT_DOUBLE,                true,  false, true},
    /// `sampleLOD` / `sampleBias` / `sampleGrad` on a `texture1d` or
    /// `texture1d_array`. Apple GPUs have no mipmap pyramid for 1D
    /// textures, so MSL has no `level()` / `bias()` overload and no
    /// `gradient1d` function exists at all. HLSL and GLSL both expose
    /// the operation.
    {"TEXTURE1D_MIP_SAMPLE",  OMEGASL_FEATURE_BIT_TEXTURE1D_MIP_SAMPLE,  true,  false, true},
    /// §1.7 — user cull distance. HLSL `SV_CullDistance` and GLSL
    /// `gl_CullDistance[]` express it; Metal has no cull-distance equivalent,
    /// so MSL is not expressible (the shader stubs and the runtime rejects
    /// pipelines that bind it). Clip distance is universal and ungated.
    {"CULL_DISTANCE",         OMEGASL_FEATURE_BIT_CULL_DISTANCE,         true,  false, true}
}};

bool isExpressible(const FeatureEntry& f, PPBackend backend) {
    switch (backend) {
        case PPBackend::HLSL: return f.hlslExpressible;
        case PPBackend::MSL:  return f.mslExpressible;
        case PPBackend::GLSL: return f.glslExpressible;
    }
    return false;
}

const FeatureEntry* lookupFeature(const std::string& name) {
    for (const auto& f : kFeatureTable) {
        if (name == f.name) return &f;
    }
    return nullptr;
}

void trimInPlace(std::string& s) {
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
}

/// The six shader-stage keywords. These are *hard* keywords in the Lexer
/// (`isKeyword`, `Lexer.cpp`), so a `TOK_KW` whose text is one of these is
/// unambiguously a shader entry point declaration — it can never be an
/// identifier. Mirrors the stage-keyword subset of `Lexer::isKeyword`.
bool isStageKeyword(const std::string& text) {
    return text == KW_VERTEX || text == KW_FRAGMENT || text == KW_COMPUTE ||
           text == KW_HULL || text == KW_DOMAIN || text == KW_MESH;
}

/// True if `path` ends in the recommended header extension `.omegaslh`.
/// Case-sensitive — the convention is exactly `.omegaslh`. Drives the
/// Phase 4 advisory: an include whose path is not a `.omegaslh` is nudged
/// toward the convention (warn-only; the include is still processed).
bool hasOmegaslhExtension(const std::string& path) {
    static const std::string ext = ".omegaslh";
    return path.size() >= ext.size() &&
           path.compare(path.size() - ext.size(), ext.size(), ext) == 0;
}

} // namespace

void Preprocessor::setBackend(PPBackend backend) {
    backendSet_ = true;
    switch (backend) {
        case PPBackend::HLSL: define("OMEGASL_BACKEND_HLSL"); break;
        case PPBackend::MSL:  define("OMEGASL_BACKEND_MSL");  break;
        case PPBackend::GLSL: define("OMEGASL_BACKEND_GLSL"); break;
    }
    /// Predefine OMEGASL_FEATURE_<NAME> for each feature expressible on
    /// the active backend. Source-level `#if defined(OMEGASL_FEATURE_X)`
    /// branches off this set; `#requires(X)` resolves the satisfied vs.
    /// unsatisfied split against this set.
    for (const auto& f : kFeatureTable) {
        if (isExpressible(f, backend)) {
            define(std::string("OMEGASL_FEATURE_") + f.name);
        }
    }
}

void Preprocessor::define(const std::string& name, const std::string& value) {
    macros_[name] = value;
}

bool Preprocessor::isDefined(const std::string& name) const {
    return macros_.find(name) != macros_.end();
}

std::string Preprocessor::process(const std::string& source, const std::string& currentPath) {
    requiredFeatures_ = 0;
    unsatisfiedFeatures_ = 0;
    return processInternal(source, currentPath, 0);
}

std::string Preprocessor::expandMacros(const std::string& line) const {
    std::string out = line;
    for (const auto& p : macros_) {
        if (p.first.empty()) continue;
        const std::string& needle = p.first;
        size_t pos = 0;
        while ((pos = out.find(needle, pos)) != std::string::npos) {
            bool atStart = (pos == 0) ||
                           (!std::isalnum(static_cast<unsigned char>(out[pos - 1])) && out[pos - 1] != '_');
            bool atEnd = (pos + needle.size() >= out.size()) ||
                         (!std::isalnum(static_cast<unsigned char>(out[pos + needle.size()])) &&
                          out[pos + needle.size()] != '_');
            if (atStart && atEnd) {
                out.replace(pos, needle.size(), p.second);
                pos += p.second.size();
            } else {
                pos += needle.size();
            }
        }
    }
    return out;
}

void Preprocessor::handleRequiresDirective(const std::string& argText) {
    /// Accept either `(F1, F2, ...)` or `F1, F2, ...`. Tokenize on
    /// commas and parens; trim whitespace on each name; each name is
    /// one of the macro suffixes from `kFeatureTable` (e.g. "RAYTRACING")
    /// or a fully-qualified `OMEGASL_FEATURE_<NAME>`.
    std::string text = argText;
    /// Strip a single surrounding pair of parens if present.
    auto firstNonSpace = text.find_first_not_of(" \t");
    if (firstNonSpace != std::string::npos && text[firstNonSpace] == '(') {
        text.erase(0, firstNonSpace + 1);
        auto rp = text.rfind(')');
        if (rp != std::string::npos) {
            text.erase(rp);
        }
    }

    size_t pos = 0;
    while (pos < text.size()) {
        size_t comma = text.find(',', pos);
        std::string token = text.substr(pos, (comma == std::string::npos ? text.size() : comma) - pos);
        trimInPlace(token);
        if (!token.empty()) {
            /// Allow either bare suffix or fully qualified macro spelling.
            std::string suffix = token;
            const std::string prefix = "OMEGASL_FEATURE_";
            if (suffix.compare(0, prefix.size(), prefix) == 0) {
                suffix.erase(0, prefix.size());
            }
            const FeatureEntry* f = lookupFeature(suffix);
            if (!f) {
                std::cerr << "warning: #requires names unknown feature `" << token
                          << "`; ignoring." << std::endl;
            } else {
                requiredFeatures_ |= f->bit;
                if (!isDefined(prefix + f->name)) {
                    unsatisfiedFeatures_ |= f->bit;
                }
            }
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
}

bool Preprocessor::evaluateIfExpression(const std::string& expr) const {
    /// Minimal expression evaluator: supports `defined(NAME)`,
    /// `defined NAME`, or a bare `NAME` (treated as `defined(NAME)`).
    /// Compound expressions (`&&`, `||`, `!`) are deliberately out of
    /// scope for this cut — `#requires` covers feature gating; source-
    /// level fallback uses the simple form.
    std::string e = expr;
    trimInPlace(e);
    if (e.empty()) return false;
    if (e.compare(0, 7, "defined") == 0) {
        e.erase(0, 7);
        trimInPlace(e);
        if (!e.empty() && e.front() == '(') {
            e.erase(0, 1);
            auto rp = e.rfind(')');
            if (rp != std::string::npos) e.erase(rp);
        }
        trimInPlace(e);
    }
    /// Strip whatever's left to a bare identifier.
    auto end = std::find_if(e.begin(), e.end(), [](unsigned char c) {
        return !std::isalnum(c) && c != '_';
    });
    e.erase(end, e.end());
    if (e.empty()) return false;
    return isDefined(e);
}

bool Preprocessor::includeDeclaresShader(const std::string& processedContent,
                                         std::string& keywordOut, unsigned& lineOut) const {
    /// Drive the real Lexer over the already-preprocessed include body.
    /// Reusing it (rather than a substring scan) gives correct skipping of
    /// line/block comments and string literals, whole-token identifier
    /// boundaries, and a line number — all for free. The scan runs on the
    /// *processed* text so a shader behind an `#if`/`#ifdef` that resolved
    /// to false for the active backend was already stripped and does not
    /// count. This is the exact lexer-driving pattern used by the
    /// `--tokens-only` path in `main.cpp`.
    std::istringstream stream(processedContent);
    Lexer lexer;
    lexer.setInputStream(&stream);
    for (Tok t = lexer.nextTok(); t.type != TOK_EOF; t = lexer.nextTok()) {
        if (t.type == TOK_KW && isStageKeyword(t.str)) {
            keywordOut = t.str;
            lineOut = t.line;
            lexer.finishTokenizeFromStream();
            return true;
        }
    }
    lexer.finishTokenizeFromStream();
    return false;
}

std::string Preprocessor::processInternal(const std::string& source, const std::string& currentPath, unsigned includeDepth) {
    if (includeDepth > kMaxIncludeDepth) return source;

    std::ostringstream out;
    std::istringstream in(source);
    std::string line;
    /// Each entry tracks whether the *enclosing* scope was already
    /// skipping when the corresponding `#if/#ifdef/#ifndef` opened, so
    /// we can restore that state on `#endif`. `branchTaken` records
    /// whether any branch in the current `#if` chain has matched —
    /// `#else` only runs when no prior branch matched.
    struct Frame {
        bool parentSkipping;
        bool branchTaken;
    };
    std::vector<Frame> stack;
    bool skipping = false;

    while (std::getline(in, line)) {
        size_t start = 0;
        while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) ++start;
        if (start >= line.size()) {
            if (!skipping) out << line << "\n";
            else if (linePreserving_) out << "\n";
            continue;
        }
        if (line[start] != '#') {
            if (!skipping) out << expandMacros(line) << "\n";
            else if (linePreserving_) out << "\n";
            continue;
        }

        /// Directive line. It produces no output of its own (an `#include`
        /// that expands content is rejected when line-preserving is on — see
        /// `setLinePreserving`), so emit a single blank line to keep output
        /// line numbers aligned 1:1 with the source for tooling.
        if (linePreserving_) out << "\n";

        size_t dirStart = start + 1;
        while (dirStart < line.size() && (line[dirStart] == ' ' || line[dirStart] == '\t')) ++dirStart;
        size_t dirEnd = dirStart;
        while (dirEnd < line.size() && line[dirEnd] != ' ' && line[dirEnd] != '\t' && line[dirEnd] != '(') ++dirEnd;
        std::string directive(line.begin() + dirStart, line.begin() + dirEnd);
        size_t argStart = dirEnd;
        while (argStart < line.size() && (line[argStart] == ' ' || line[argStart] == '\t')) ++argStart;
        std::string argText(line.begin() + argStart, line.end());

        if (directive == "define") {
            if (skipping) continue;
            size_t nameEnd = 0;
            while (nameEnd < argText.size() &&
                   (std::isalnum(static_cast<unsigned char>(argText[nameEnd])) || argText[nameEnd] == '_')) {
                ++nameEnd;
            }
            std::string name = argText.substr(0, nameEnd);
            std::string value = argText.substr(nameEnd);
            trimInPlace(value);
            if (!name.empty()) define(name, value.empty() ? std::string("1") : value);
        }
        else if (directive == "ifdef") {
            std::string name = argText;
            trimInPlace(name);
            bool cond = isDefined(name);
            stack.push_back({skipping, cond});
            skipping = skipping || !cond;
        }
        else if (directive == "ifndef") {
            std::string name = argText;
            trimInPlace(name);
            bool cond = !isDefined(name);
            stack.push_back({skipping, cond});
            skipping = skipping || !cond;
        }
        else if (directive == "if") {
            bool cond = evaluateIfExpression(argText);
            stack.push_back({skipping, cond});
            skipping = skipping || !cond;
        }
        else if (directive == "elif") {
            if (stack.empty()) continue;
            auto& frame = stack.back();
            if (frame.parentSkipping) {
                skipping = true;
            } else if (frame.branchTaken) {
                skipping = true;
            } else {
                bool cond = evaluateIfExpression(argText);
                if (cond) {
                    frame.branchTaken = true;
                    skipping = false;
                } else {
                    skipping = true;
                }
            }
        }
        else if (directive == "else") {
            if (stack.empty()) continue;
            auto& frame = stack.back();
            if (frame.parentSkipping) {
                skipping = true;
            } else if (frame.branchTaken) {
                skipping = true;
            } else {
                frame.branchTaken = true;
                skipping = false;
            }
        }
        else if (directive == "endif") {
            if (!stack.empty()) {
                skipping = stack.back().parentSkipping;
                stack.pop_back();
            }
        }
        else if (directive == "requires") {
            /// Layer 1 of the Backend Feature Gating system (§14.1):
            /// declare the runtime features this file's shaders need.
            /// Per the user's modification: never hard-fail at compile
            /// — record the bits, let the active backend's expressibility
            /// decide whether the body transpiles normally or as a stub.
            if (skipping) continue;
            if (!backendSet_) {
                std::cerr << "warning: #requires used without an active backend; "
                             "feature bits ignored." << std::endl;
                continue;
            }
            handleRequiresDirective(argText);
        }
        else if (directive == "include" && !skipping) {
            if (rejectIncludes_) {
                // Runtime-compiled source has no file-system context for
                // resolving include paths reliably (and propagating one
                // through the public `OmegaSLCompiler::Source` API would
                // diverge it from "single self-contained source string").
                // Fail loud so callers concatenate the dependency instead
                // of seeing a silently-dropped include surface later as a
                // missing-symbol parser error.
                std::cerr << "error: #include is not allowed in runtime-compiled "
                             "OmegaSL source — concatenate the dependency into "
                             "the source string passed to OmegaSLCompiler instead. "
                             "(directive: `" << line << "`)" << std::endl;
                hasErrors_ = true;
                continue;
            }
            size_t q = line.find('"', argStart);
            if (q != std::string::npos) {
                size_t q2 = line.find('"', q + 1);
                if (q2 != std::string::npos) {
                    std::string incPath(line.begin() + q + 1, line.begin() + q2);
                    std::string fullPath = currentPath.empty() ? incPath : (currentPath + "/" + incPath);
                    std::ifstream incFile(fullPath);
                    if (incFile) {
                        /// Phase 4 advisory: a header — a file included for its
                        /// shared declarations — should be named `*.omegaslh`.
                        /// A `.omegasl` is a translation unit that owns shader
                        /// entry points. Nudge toward the convention but keep
                        /// processing (warn-only): hard-failing here would break
                        /// any project that includes a file of pure declarations
                        /// under a different extension. The real guard against a
                        /// header carrying a shader is the rejection below.
                        if (!hasOmegaslhExtension(incPath)) {
                            std::cerr << "warning: `#include \"" << incPath
                                      << "\"` does not use the recommended `.omegaslh` header "
                                         "extension. Headers (files included for their shared "
                                         "declarations) should be named `*.omegaslh`; a "
                                         "`.omegasl` is a translation unit that owns shader "
                                         "entry points. (Advisory — the include is still "
                                         "processed.)" << std::endl;
                        }
                        std::string incContent((std::istreambuf_iterator<char>(incFile)),
                                               std::istreambuf_iterator<char>());
                        incFile.close();
                        size_t slash = fullPath.find_last_of("/\\");
                        std::string incDir = (slash != std::string::npos) ? fullPath.substr(0, slash) : "";
                        std::string processedInclude = processInternal(incContent, incDir, includeDepth + 1);
                        /// A header is textually inlined into every including
                        /// translation unit. A shader entry point declared in
                        /// one would therefore be compiled into each unit and
                        /// collide by name at link time (and bloat the output).
                        /// Reject it loudly here — name the header, the line,
                        /// and the offending stage keyword — and drop the
                        /// content rather than silently inline a duplicate that
                        /// would surface as a cryptic link-time collision later.
                        /// (Nested includes are covered: an inner header's
                        /// shader is caught and stripped by the recursive
                        /// `processInternal` above before this outer scan runs.)
                        std::string stageKeyword;
                        unsigned stageLine = 0;
                        if (includeDeclaresShader(processedInclude, stageKeyword, stageLine)) {
                            std::cerr << "error: included header `" << fullPath
                                      << "` declares a shader entry point (`"
                                      << stageKeyword << "` at line " << stageLine
                                      << "). Shader entry points must live in a translation "
                                         "unit (`.omegasl`), not in an `#include`d header "
                                         "(`.omegaslh`): a header is inlined into every "
                                         "including unit, so a shader in it is compiled "
                                         "multiple times and collides at link time. Move the `"
                                      << stageKeyword << "` entry point into a `.omegasl` "
                                         "translation unit and keep only shared declarations "
                                         "(structs, resources, helper functions) in the header."
                                      << std::endl;
                            hasErrors_ = true;
                        } else {
                            out << processedInclude;
                        }
                    }
                }
            }
        }
    }
    return out.str();
}

} // namespace omegasl
