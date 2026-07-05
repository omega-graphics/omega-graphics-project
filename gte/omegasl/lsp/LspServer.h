#ifndef OMEGASL_LSP_SERVER_H
#define OMEGASL_LSP_SERVER_H

#include <iostream>
#include <string>
#include <map>

#include "Analysis.h"

namespace OmegaCommon { class JSON; }

/// The OmegaSL language server: LSP over stdio (JSON-RPC 2.0, `Content-Length`
/// framing). Owns the open-document store and routes each request to a handler;
/// the compiler work lives entirely behind `Analyzer`.
namespace omegasl {
namespace lsp {

    class LspServer {
    public:
        LspServer(std::istream & in, std::ostream & out, Analyzer & analyzer);

        /// Run the message loop until the client sends `exit`. Returns the
        /// process exit code (0 if a `shutdown` preceded `exit`, else 1, per
        /// the LSP spec).
        int run();

    private:
        std::istream & in_;
        std::ostream & out_;
        Analyzer & analyzer_;

        /// Open documents: URI → full current text (full text sync).
        std::map<std::string, std::string> documents_;
        /// Most recent analysis per URI, reused by documentSymbol / hover /
        /// completion without re-running the compiler.
        std::map<std::string, AnalysisResult> analyses_;

        /// Compile-command discovery cache: a document directory → the
        /// `omegasl_commands.json` that governs it (empty string = none found),
        /// so the upward search runs once per directory, not per keystroke.
        std::map<std::string, std::string> dbPathForDir_;
        /// Parsed compile databases, keyed by their `omegasl_commands.json`
        /// path: absolute source file → its `-I` include directories. v1 does
        /// not hot-reload a database that changes on disk after first load.
        std::map<std::string, std::map<std::string, std::vector<std::string>>> dbByPath_;

        bool shutdownRequested_ = false;

        /// Read one framed message body. Returns false at EOF / stream error.
        bool readMessage(std::string & body);
        /// Frame and write one JSON value to the client.
        void writeMessage(OmegaCommon::JSON & message);

        /// Route a parsed message; returns true when it was `exit`.
        bool dispatch(OmegaCommon::JSON & message);

        void sendResult(const OmegaCommon::JSON & id, OmegaCommon::JSON result);
        void sendError(const OmegaCommon::JSON & id, int code, const std::string & message);

        void handleInitialize(const OmegaCommon::JSON & id, OmegaCommon::JSON & params);
        void handleDidOpen(OmegaCommon::JSON & params);
        void handleDidChange(OmegaCommon::JSON & params);
        void handleDidClose(OmegaCommon::JSON & params);
        void handleDocumentSymbol(const OmegaCommon::JSON & id, OmegaCommon::JSON & params);
        void handleHover(const OmegaCommon::JSON & id, OmegaCommon::JSON & params);
        void handleCompletion(const OmegaCommon::JSON & id, OmegaCommon::JSON & params);

        /// Include search directories for a document, from the governing
        /// `omegasl_commands.json` (discovered by walking up from the file's
        /// directory). Empty when the document has no path or no matching
        /// compile-command entry. Results are cached (see `dbPathForDir_` /
        /// `dbByPath_`).
        std::vector<std::string> includeDirsForDocument(const std::string & docPath);

        /// Re-analyze `uri` from the document store and publish diagnostics.
        void analyzeAndPublish(const std::string & uri);
        void publishDiagnostics(const std::string & uri, const AnalysisResult & result);
    };

} // namespace lsp
} // namespace omegasl

#endif // OMEGASL_LSP_SERVER_H
