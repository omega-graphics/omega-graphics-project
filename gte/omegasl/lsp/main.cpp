#include "Analysis.h"
#include "LspServer.h"

#include <iostream>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

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
#ifdef _WIN32
    /// The CRT opens stdin/stdout in text mode by default on Windows, which
    /// rewrites a bare '\n' to "\r\n" on write (and can drop bytes on read).
    /// `LspServer` frames every message with a literal "\r\n\r\n" and a
    /// byte-exact Content-Length, so text-mode translation corrupts the very
    /// first response and the client never completes the handshake. Binary
    /// mode must be set before any stdio traffic -- first thing in main().
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    /// Untie cin from cout: we drive the framing by hand and flush explicitly
    /// after every message, so the default tie (which would flush cout before
    /// every cin read) is just overhead.
    std::ios::sync_with_stdio(true);
    std::cin.tie(nullptr);

    omegasl::lsp::Analyzer analyzer;
    omegasl::lsp::LspServer server(std::cin, std::cout, analyzer);
    return server.run();
}
