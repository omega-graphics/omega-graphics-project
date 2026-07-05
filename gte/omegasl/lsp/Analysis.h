#ifndef OMEGASL_LSP_ANALYSIS_H
#define OMEGASL_LSP_ANALYSIS_H

#include <string>
#include <vector>
#include <map>

/// Compiler bridge for the OmegaSL language server.
///
/// `Analyzer` is the *only* part of the server that touches the OmegaSL
/// front-end. It drives the real `Preprocessor` → `Parser` → `Sem` pipeline in
/// frontend-only mode (no code generation, no shader-toolchain invocation) and
/// turns its outputs — the `DiagnosticEngine` errors and the parsed AST — into
/// the editor-facing data the protocol layer serves: diagnostics, a top-level
/// symbol outline, and a symbol index for hover / completion.
///
/// Because the diagnostics come straight from the compiler's own
/// `DiagnosticEngine`, what the editor underlines is exactly what `omegaslc`
/// would report — no second, drifting re-implementation of the language.
namespace omegasl {
namespace lsp {

    /// A half-open source range in LSP coordinates: 0-based line, 0-based
    /// character offset. (OmegaSL tokens are 1-based line / 0-based column;
    /// the analyzer converts.)
    struct Range {
        unsigned startLine = 0;
        unsigned startChar = 0;
        unsigned endLine = 0;
        unsigned endChar = 0;
    };

    /// LSP `DiagnosticSeverity`. The front-end only produces hard errors
    /// today, so every diagnostic is `Error`; the field exists so a future
    /// warning channel (e.g. the portability scanner) can downgrade.
    enum class Severity : int {
        Error = 1,
        Warning = 2,
        Information = 3,
        Hint = 4
    };

    struct Diagnostic {
        Range range;
        std::string message;
        Severity severity = Severity::Error;
    };

    /// Kind of a top-level declaration. Maps onto an LSP `SymbolKind` in the
    /// protocol layer and selects the hover/completion presentation.
    enum class SymbolKind : int {
        Struct,
        Function,
        VertexShader,
        FragmentShader,
        ComputeShader,
        HullShader,
        DomainShader,
        MeshShader,
        Buffer,
        Texture,
        Sampler,
        Constant
    };

    /// One top-level declaration surfaced to the editor.
    struct Symbol {
        std::string name;
        SymbolKind kind = SymbolKind::Function;
        /// Short descriptor for the document-symbol list (e.g. `vertex shader`,
        /// `buffer<MyVertex>`). Shown dimmed after the name in the outline.
        std::string detail;
        /// One- or multi-line OmegaSL spelling for the hover code block
        /// (e.g. `vertex VertexRaster myVertex(uint vid)`, or a struct with its
        /// fields). Rendered inside a fenced ```omegasl block on hover.
        std::string signature;
        /// Location of the declaration's name token. With only the name token
        /// available from the parser, `range` and `selectionRange` coincide.
        Range range;
    };

    struct AnalysisResult {
        std::vector<Diagnostic> diagnostics;
        /// Top-level declarations in source order, for the document outline.
        std::vector<Symbol> symbols;
        /// Top-level declarations keyed by name, for hover and completion.
        std::map<std::string, Symbol> index;
    };

    /// Owns the global builtin catalog (`ast::builtins`) for the server's
    /// lifetime: exactly one `Analyzer` may exist per process. `analyze` is
    /// re-entrant across documents and allocates a fresh parser/semantic state
    /// per call.
    class Analyzer {
    public:
        Analyzer();
        ~Analyzer();

        Analyzer(const Analyzer &) = delete;
        Analyzer & operator=(const Analyzer &) = delete;

        /// Run the front-end over `text` and return diagnostics + symbols.
        /// `text` is the full current document contents (the server uses full
        /// text sync). Never throws; a malformed document yields diagnostics.
        ///
        /// `documentPath` is the document's absolute filesystem path (decoded
        /// from its `file://` URI), or empty when the client gave no path. Its
        /// directory anchors relative `#include "foo.omegaslh"` resolution.
        /// `includeDirs` are extra `-I` search directories (from a discovered
        /// `omegasl_commands.json`). When either is available, `#include`s are
        /// resolved and their declarations participate in analysis; diagnostics
        /// and symbols are mapped back onto the editor buffer and any that
        /// originate inside an included header are dropped from the main
        /// document (header symbols still feed hover/completion via the index).
        /// With neither, `#include` is rejected (as before), since there is no
        /// filesystem anchor to resolve against.
        AnalysisResult analyze(const std::string & text,
                               const std::string & documentPath,
                               const std::vector<std::string> & includeDirs);
    };

} // namespace lsp
} // namespace omegasl

#endif // OMEGASL_LSP_ANALYSIS_H
