#include "Analysis.h"
#include "LspServer.h"

#include <iostream>

/// Entry point for `omegasl-lsp`, the OmegaSL language server.
///
/// The server speaks LSP over stdio: it reads `Content-Length`-framed JSON-RPC
/// requests on stdin and writes responses / notifications on stdout. stderr is
/// left free for human-readable logging (the compiler front-end and the
/// preprocessor write diagnostics there).
///
/// `Analyzer` owns the global builtin catalog for the process lifetime, so it
/// is constructed once, here, and shared across every document the server
/// analyzes.
int main() {
    /// Untie cin from cout: we drive the framing by hand and flush explicitly
    /// after every message, so the default tie (which would flush cout before
    /// every cin read) is just overhead.
    std::ios::sync_with_stdio(true);
    std::cin.tie(nullptr);

    omegasl::lsp::Analyzer analyzer;
    omegasl::lsp::LspServer server(std::cin, std::cout, analyzer);
    return server.run();
}
