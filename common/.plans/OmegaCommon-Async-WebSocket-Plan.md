# OmegaCommon Async I/O + WebSocket Plan

## Overview

This plan adds two new subsystems to OmegaCommon, both landing in the
new **`OmegaCommonASIO`** binary defined by
`OmegaCommon-Binary-Split-Plan.md`:

- **`io.h`** — an async I/O context (event loop + thread pool) that other
  OmegaCommon networking subsystems can sit on top of.
- **`websocket.h`** — a header-PIMPL'd WebSocket client and server, both
  TLS-capable, both driven by `IoContext`.

Both are backed by **Boost.Asio + Boost.Beast** vendored privately under
`common/deps/boost` via `AUTOMDEPS`. Neither header leaks any Boost type
across the OmegaCommonASIO DLL boundary; public symbols use the
`OMEGACOMMON_ASIO_EXPORT` macro introduced by the split plan.

The existing curl/WinHTTP `HttpClientContext` in `net.h` (now in
OmegaCommon.Core) is **untouched**. This plan adds a parallel async
transport surface; it does not migrate HTTP off curl.

## Prerequisite

`OmegaCommon-Binary-Split-Plan.md` Phase 3 must land first — it creates
the empty `OmegaCommonASIO` SHARED target this plan fills. The two
plans were authored together; without the split, all references to
`OmegaCommonASIO` below would have to fall back to the legacy single
`OmegaCommon` target.

## Decisions (locked in before writing)

| # | Question | Answer |
|---|----------|--------|
| 1 | Callback style for WebSocket events | `std::function` callbacks (matches existing OmegaCommon patterns) |
| 2 | Replace curl HTTP? | No — curl/WinHTTP `HttpClientContext` stays as-is |
| 3 | WebSocket library | Boost.Beast (server + client); Boost.Asio used as the reactor under both Beast and our public `IoContext` |
| 4 | Version pinning | Pin tags — no float to HEAD |

Vendoring all-Boost (rather than Beast over standalone-asio) avoids the
`BOOST_ASIO_STANDALONE` straddle and lets Beast's idiomatic `tcp_stream` /
`ssl_stream` usage work without per-include workarounds.

## Vendoring strategy

### AUTOMDEPS additions (`common/AUTOMDEPS`)

Add two entries; Boost is fetched as a release archive (avoids
`git submodule update --init --recursive` which the existing `type:"git"`
path does not invoke), Beast ships inside the Boost archive so no second
entry is required.

```json
{
    "name": "boost",
    "type": "archive",
    "url": "https://archives.boost.io/release/1.86.0/source/boost_1_86_0.tar.gz",
    "archive_name": "boost_1_86_0.tar.gz",
    "dest": "$(third_party_dest)/boost",
    "strip_components": 1,
    "exports": {
        "source_root": "$(third_party_dest)/boost"
    }
}
```

Pin: **Boost 1.86.0** (released Aug 2024; Beast + Asio API has been
stable across the 1.83 → 1.86 window, and 1.86 is the version where
`boost::system` is fully header-only in our usage). Bump deliberately,
not on autoupdate.

### Why archive, not git

- Boost's monorepo uses one git submodule per module; a flat clone leaves
  every `libs/*/include` empty. The existing `clone.py` in
  `autom/tools/autom-deps/` does not recurse submodules.
- Release archives are pre-flattened (all module headers present at
  `boost/`), 140 MB compressed, single deterministic SHA.
- Matches the `pcre2` pattern already used in `common/AUTOMDEPS`.

### License posture

Boost is BSL-1.0 — compatible with this repo. Add a short note to
`README.md` under third-party attributions when Phase 1 lands.

### Header-only consumption

Boost.Beast is header-only. Boost.Asio is header-only when **not** using
`BOOST_ASIO_SEPARATE_COMPILATION`. Boost.System (Asio's only hard
dependency) is header-only by default in 1.86. Net result: **no Boost
sources need to be compiled** — the entry is purely an include-path
provider.

Defines applied to `OmegaCommonASIO` (PRIVATE):

- `BOOST_ASIO_NO_DEPRECATED` — drop legacy API names
- `BOOST_BEAST_USE_STD_STRING_VIEW` — interop with our `StrRef` family
- `BOOST_ASIO_DISABLE_BOOST_REGEX` — we have our own (PCRE2)
- `BOOST_ASIO_HAS_STD_INVOKE_RESULT` — C++17 (matches AGENTS.md standard)
- `_WIN32_WINNT=0x0A00` on Windows (Win10+) so Asio picks the modern
  IOCP path; mirrors what existing `win/net-win.cpp` already assumes

## Public API

Both headers are PIMPL'd in the same shape as `Semaphore` / `Pipe` in
`multithread.h`: a forward-declared `struct Impl;` plus a
`std::unique_ptr<Impl>`. No `<boost/...>` include ever appears in
`include/omega-common/*.h`.

### `include/omega-common/io.h`

```cpp
#ifndef OMEGA_COMMON_IO_H
#define OMEGA_COMMON_IO_H

#include "utils.h"
#include <chrono>
#include <functional>
#include <memory>

namespace OmegaCommon {

    /// Async I/O reactor. Wraps a boost::asio::io_context.
    /// All public OmegaCommon async subsystems (WebSocket today,
    /// future TCP/UDP/timers) bind to an IoContext at construction.
    class OMEGACOMMON_EXPORT IoContext {
        struct Impl;
        std::unique_ptr<Impl> impl;
        friend class WebSocketClient;
        friend class WebSocketServer;
    public:
        IoContext();
        IoContext(const IoContext &) = delete;
        IoContext &operator=(const IoContext &) = delete;
        IoContext(IoContext &&) noexcept;
        IoContext &operator=(IoContext &&) noexcept;

        /// Block running handlers until stop() is called or no work remains.
        void run();
        /// Run at most one handler. Returns number of handlers dispatched (0 or 1).
        size_t runOne();
        /// Run all currently-ready handlers without blocking.
        size_t poll();
        /// Signal run()/runOne() loops to return as soon as possible.
        void stop();
        /// Reset after a stop() so the context can be run() again.
        void restart();

        /// Schedule a callable to run on a thread currently inside run().
        void post(std::function<void()> fn);
        /// Schedule a callable to run after a delay.
        void postAfter(std::chrono::milliseconds delay, std::function<void()> fn);

        ~IoContext();
    };

    /// Spawns N worker threads, each calling IoContext::run() in a loop.
    /// Joining happens on destruction.
    class OMEGACOMMON_EXPORT IoThreadPool {
        struct Impl;
        std::unique_ptr<Impl> impl;
    public:
        IoThreadPool(IoContext &ctx, size_t threadCount);
        IoThreadPool(const IoThreadPool &) = delete;
        IoThreadPool &operator=(const IoThreadPool &) = delete;
        IoThreadPool(IoThreadPool &&) noexcept;
        IoThreadPool &operator=(IoThreadPool &&) noexcept;
        /// Stop the IoContext and join all workers. Idempotent.
        void shutdown();
        ~IoThreadPool();
    };

}

#endif
```

### `include/omega-common/websocket.h`

```cpp
#ifndef OMEGA_COMMON_WEBSOCKET_H
#define OMEGA_COMMON_WEBSOCKET_H

#include "utils.h"
#include "io.h"
#include <cstdint>
#include <functional>
#include <future>
#include <memory>

namespace OmegaCommon {

    enum class WebSocketOpcode : std::uint8_t {
        Text = 0x1,
        Binary = 0x2
    };

    struct WebSocketMessage {
        WebSocketOpcode opcode;
        Vector<std::uint8_t> payload;
        String text() const {
            return payload.empty()
                ? String{}
                : String(reinterpret_cast<const char *>(payload.data()), payload.size());
        }
    };

    /// TLS configuration. Empty PEM strings mean "use system defaults".
    /// Mirrors HttpTlsConfig in net.h on purpose.
    struct WebSocketTlsConfig {
        String caBundlePem;
        String clientCertPem;
        String clientKeyPem;
        bool verifyPeer = true;
    };

    /// Optional handshake customization.
    struct WebSocketConnectOptions {
        Vector<std::pair<String, String>> requestHeaders;
        Vector<String> subProtocols;
        std::chrono::milliseconds handshakeTimeout{15000};
    };

    /// Per-connection event callbacks. All run on a thread inside IoContext::run().
    struct WebSocketCallbacks {
        std::function<void()> onOpen;
        std::function<void(WebSocketMessage)> onMessage;
        std::function<void(std::uint16_t code, const String &reason)> onClose;
        std::function<void(const String &message)> onError;
    };

    class OMEGACOMMON_EXPORT WebSocketClient {
        struct Impl;
        std::unique_ptr<Impl> impl;
    public:
        explicit WebSocketClient(IoContext &ctx);
        explicit WebSocketClient(IoContext &ctx, WebSocketTlsConfig tls);
        WebSocketClient(const WebSocketClient &) = delete;
        WebSocketClient &operator=(const WebSocketClient &) = delete;
        WebSocketClient(WebSocketClient &&) noexcept;
        WebSocketClient &operator=(WebSocketClient &&) noexcept;

        void setCallbacks(WebSocketCallbacks cbs);

        /// Resolves to void on a successful handshake, throws on failure.
        /// Accepts ws:// and wss:// schemes; wss:// requires a TLS-configured client.
        std::future<void> connect(StrRef url, WebSocketConnectOptions opts = {});

        /// Returns immediately; actual send is posted to the io context.
        void sendText(StrRef text);
        void sendBinary(ArrayRef<std::uint8_t> data);

        /// Initiate graceful close. onClose fires when the peer ack'd.
        void close(std::uint16_t code = 1000, StrRef reason = {});

        ~WebSocketClient();
    };

    /// Per-connection handle handed to the server's accept callback.
    class OMEGACOMMON_EXPORT WebSocketConnection {
        struct Impl;
        std::unique_ptr<Impl> impl;
        friend class WebSocketServer;
    public:
        explicit WebSocketConnection(std::unique_ptr<Impl> impl);
        WebSocketConnection(const WebSocketConnection &) = delete;
        WebSocketConnection &operator=(const WebSocketConnection &) = delete;
        WebSocketConnection(WebSocketConnection &&) noexcept;
        WebSocketConnection &operator=(WebSocketConnection &&) noexcept;
        void sendText(StrRef text);
        void sendBinary(ArrayRef<std::uint8_t> data);
        void close(std::uint16_t code = 1000, StrRef reason = {});
        String remoteEndpoint() const;
        ~WebSocketConnection();
    };

    struct WebSocketServerOptions {
        StrRef host = "0.0.0.0";
        std::uint16_t port = 0;
        size_t backlog = 16;
        /// Optional TLS termination. Leave caBundlePem etc. empty for plain ws://.
        WebSocketTlsConfig tls;
        bool useTls = false;
    };

    class OMEGACOMMON_EXPORT WebSocketServer {
        struct Impl;
        std::unique_ptr<Impl> impl;
    public:
        explicit WebSocketServer(IoContext &ctx);
        WebSocketServer(const WebSocketServer &) = delete;
        WebSocketServer &operator=(const WebSocketServer &) = delete;
        WebSocketServer(WebSocketServer &&) noexcept;
        WebSocketServer &operator=(WebSocketServer &&) noexcept;

        using OnAccept = std::function<WebSocketCallbacks(WebSocketConnection &)>;

        /// Begin accepting. Each new connection invokes onAccept on an io thread;
        /// the returned WebSocketCallbacks govern that connection's lifecycle.
        void start(WebSocketServerOptions opts, OnAccept onAccept);
        void stop();

        ~WebSocketServer();
    };

}

#endif
```

### PIMPL discipline (enforced, not aspirational)

- No `<boost/...>` include in any header under `include/omega-common/`.
- Public callback types take only OmegaCommon / std types.
- All `boost::system::error_code` translation happens in the `.cpp`;
  errors surface to the caller as `String` messages (via the
  `onError` callback or a `std::future`'s exception).
- Move constructors are `noexcept` so containers behave.

## TLS — reuse OmegaCommonCore's OpenSSL build

Boost.Beast's `ssl_stream` uses `boost::asio::ssl::context`, which is a
thin wrapper over OpenSSL's `SSL_CTX`. OpenSSL is already built via
`add_third_party` in `common/CMakeLists.txt` and consumed by
OmegaCommon.Core. OmegaCommonASIO links Core PUBLIC and therefore
inherits the `ssl` / `crypto` imported targets — **no new TLS
dependency, no re-link of OpenSSL**.

PEM strings on `WebSocketTlsConfig` are loaded into `SSL_CTX` using
`SSL_CTX_use_certificate_chain_mem` and friends — never written to a
tempfile, matching what `crypto.h`'s TLS surface already does.

## CMake wiring (`common/CMakeLists.txt`)

The split plan's Phase 3 creates an empty `OmegaCommonASIO` SHARED
target. This plan's Phase 1b appends to that target's existing block:

```cmake
# --- Boost (header-only consumption: Beast + Asio + System) ---
if(DEFINED OMEGACOMMON_BOOST_ROOT)
    set(BOOST_SOURCE_DIR "${OMEGACOMMON_BOOST_ROOT}")
else()
    set(BOOST_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/boost")
endif()
if(NOT EXISTS "${BOOST_SOURCE_DIR}/boost/version.hpp")
    message(FATAL_ERROR
        "Boost headers not found at ${BOOST_SOURCE_DIR}. Run autom-deps.")
endif()
target_include_directories("OmegaCommonASIO" PRIVATE "${BOOST_SOURCE_DIR}")
target_compile_definitions("OmegaCommonASIO" PRIVATE
    BOOST_ASIO_NO_DEPRECATED
    BOOST_ASIO_DISABLE_BOOST_REGEX
    BOOST_BEAST_USE_STD_STRING_VIEW)
if(WIN32)
    target_compile_definitions("OmegaCommonASIO" PRIVATE _WIN32_WINNT=0x0A00)
    target_link_libraries("OmegaCommonASIO" PRIVATE ws2_32 mswsock)
endif()
```

Per-platform link additions for OmegaCommonASIO:

- **Linux**: Core already links `pthread`; ASIO inherits it via PUBLIC. No
  extra link line.
- **macOS**: kqueue is in `libSystem`; no extra framework.
- **Windows**: `ws2_32` + `mswsock` (the `WSARecvMsg` / `AcceptEx` paths
  Asio uses on IOCP). Core already pulls `ws2_32` for WinHTTP, so the
  add here is the `mswsock` delta.

Beast / Asio TLS picks up the `ssl` / `crypto` imported targets via
`OmegaCommonASIO`'s PUBLIC link to OmegaCommon.Core — no explicit
re-link is needed in this target's block.

Sources under `src/`:

```
src/io.cpp             # IoContext + IoThreadPool impl, all platforms
src/websocket.cpp      # WebSocketClient + Server + Connection impl,
                       # uses boost::beast::websocket::stream over either
                       # tcp_stream or ssl_stream depending on scheme/opts
```

No platform-specific split needed — Asio's reactor abstracts the platform
delta, mirroring the way curl abstracts the HTTP delta in `net-curl.cpp`.

## Implementation phases

Each phase ends with a buildable, testable increment. Phase numbers map
to commits/PRs.

### Phase 1 — Vendoring + skeleton (`~80 LOC`)

Pre-req: split plan Phase 3 has landed — the empty `OmegaCommonASIO`
target exists and produces a stub DLL.

- `1a` Add Boost entry to `common/AUTOMDEPS`. Run `autom-deps`, verify
  `common/deps/boost/boost/version.hpp` and
  `common/deps/boost/boost/beast.hpp` both exist.
- `1b` Append the Boost include + defines block (above) to the
  `OmegaCommonASIO` section of `common/CMakeLists.txt`. Add
  `ws2_32 mswsock` to ASIO's Windows link line.
- `1c` Create `include/omega-common/io.h` and
  `include/omega-common/websocket.h` exactly as in this doc, with
  `OMEGACOMMON_ASIO_EXPORT` on the public types.
- `1d` Replace the `src/asio_stub.cpp` placeholder created by split
  Phase 3 with real stubs `src/io.cpp` and `src/websocket.cpp` that
  define the PIMPL `Impl` struct + ctor/dtor only (no behavior).
- Verifies: Linux + Windows full build of OmegaCommonASIO clean, no
  new symbols exported except the new class shells.

**Per AGENTS.md small-feature exception, 1a–1d is a single 80-LOC change
— no further sub-bullets.**

### Phase 2 — IoContext implementation (`~250 LOC`)

- `2a` Implement `IoContext::run/runOne/poll/stop/restart/post/postAfter`
  in terms of `boost::asio::io_context`. `postAfter` uses `steady_timer`.
- `2b` Implement `IoThreadPool` — N `std::thread`s each call `ctx.run()`.
  Joinable in dtor; `shutdown()` is idempotent (sets a flag, calls
  `ctx.stop()`, joins).
- `2c` Add `common/tests/io_test.cpp` (new dir if absent; mirror layout
  of `common/assetc/tests`). Tests:
  - `post N callables, run() drains all of them`
  - `postAfter delivers after the requested delay (±50ms)`
  - `stop() makes run() return promptly even with pending work`
  - `IoThreadPool with N=4 runs handlers on multiple threads`
- CMake: register `io_test` via `add_omega_graphics_tool`.

### Phase 3 — WebSocket client (`~400 LOC`)

- `3a` Plain-text path: `WebSocketClient` over
  `boost::beast::websocket::stream<boost::beast::tcp_stream>`. URL parser
  splits `ws://host:port/path`. Resolver → connect → handshake → async
  read loop posting `onMessage`. `sendText/sendBinary` post into the io
  context, never block the caller.
- `3b` TLS path: same but over
  `boost::beast::websocket::stream<ssl_stream<tcp_stream>>`.
  `WebSocketTlsConfig` PEM strings are loaded into the `ssl::context` in
  memory (no tempfiles), matching the pattern in `crypto.h`.
- `3c` Handshake timeout via `expires_after` on the underlying
  `tcp_stream`. Close handshake honours `close(code, reason)`.
- `3d` Test in `common/tests/websocket_client_test.cpp`:
  - Stand up an in-process Beast echo server on a random port (raw Beast,
    not our `WebSocketServer` — that lands in Phase 4).
  - Connect, send text, assert echoed text arrives.
  - Send binary, assert echoed bytes arrive.
  - Initiate close, assert `onClose` fires with code 1000.
  - **No external network** — loopback only, so the test is CI-safe.

### Phase 4 — WebSocket server (`~350 LOC`)

- `4a` `WebSocketServer::start` binds an `boost::asio::ip::tcp::acceptor`,
  spawns async accept. Each accepted socket is handed to a `Session`
  PIMPL that owns the Beast stream + per-connection callbacks returned
  from the `OnAccept` lambda.
- `4b` Plain + TLS variants share the accept path; TLS performs the SSL
  handshake before the WebSocket handshake.
- `4c` `WebSocketConnection::sendText/sendBinary/close` post into the
  owning io context (no blocking caller threads).
- `4d` Test in `common/tests/websocket_server_test.cpp`:
  - Start `WebSocketServer` on port 0 (kernel picks), retrieve bound
    port, connect a `WebSocketClient` to it.
  - Round-trip text + binary.
  - Multiple concurrent connections (4 clients), assert per-connection
    isolation of callbacks.
  - TLS variant uses a throwaway self-signed cert minted via our
    existing `crypto.h` (X.509 surface already exists).

### Phase 5 — Wrapgen + docs (`~150 LOC`)

- `5a` Add `io.h` and `websocket.h` to `OmegaCommon.owrap` test surface
  so the C-binding sweep covers the new headers.
- `5b` Sphinx page `common/docs/source/Async-IO-WebSocket.rst` —
  long-form prose per AGENTS.md docs rule, with a verified copy-paste
  WebSocket client example. Cross-link from `common/docs/source/index.rst`.
- `5c` Update `common/.plans/OmegaCommon-Completion-Plan.md` module
  status table to mark Async I/O and WebSocket as `Completed`.

## Test strategy summary

| Phase | Test file | Network scope | Runtime |
|-------|-----------|---------------|---------|
| 2 | `io_test.cpp` | none | <1s |
| 3 | `websocket_client_test.cpp` | loopback only | <2s |
| 4 | `websocket_server_test.cpp` | loopback only | <2s |

All tests register under the existing `add_omega_graphics_tool` /
`add_subdirectory(tests)` pattern and gate on
`NOT CMAKE_CROSSCOMPILING`, matching `common/assetc/tests`.

## Risks and tradeoffs

| Risk | Mitigation |
|------|------------|
| Boost archive is ~140 MB compressed — slow first `autom-deps` run | One-time cost, vendored locally. Document in README. |
| Header-heavy Beast inflates OmegaCommon compile time | `boost::beast` + `boost::asio` are only included in `io.cpp` and `websocket.cpp` — public headers stay clean. Compile-time impact bounded to two TUs. |
| Asio's `io_context` is not safe to share across `fork()` | Document. ChildProcess users already understand the constraint. |
| `BOOST_BEAST_USE_STD_STRING_VIEW` requires C++17 `string_view` ABI compat across the OmegaCommon DLL boundary | We're C++17 everywhere already (AGENTS.md). Fine. |
| Phase 4 self-signed cert generation depends on `crypto.h`'s X.509 surface being complete on all platforms | Already marked `Completed` in the completion plan. Verify on Windows before Phase 4. |
| `_WIN32_WINNT=0x0A00` could conflict if a consumer of OmegaCommon defines it lower | We define it `PRIVATE`, so it only affects OmegaCommon's TUs. No public-header leak. |

## Out of scope (explicit non-goals)

- Replacing `HttpClientContext` (curl/WinHTTP). Curl HTTP stays.
- An async HTTP client built on Beast. Possible follow-up plan; not here.
- HTTP/2 or HTTP/3. Beast does HTTP/1.1; that's enough for WebSocket
  upgrades.
- WebSocket permessage-deflate compression. Beast supports it; we leave
  it off for v1 and turn it on behind a config flag in a follow-up if
  needed.
- Coroutine API. Repo is C++17 (AGENTS.md). Revisit if/when the standard
  bumps.

## Open items (ask before Phase 1 ships)

- Confirm Boost **1.86.0** is the version to pin, or pick a different
  tag (1.85 is the prior stable, 1.87 was released Dec 2024). The
  plan assumes 1.86.0.
- Confirm test target naming follows the existing
  `add_omega_graphics_tool("io-test" ...)` pattern, or whether you want
  these grouped under a single `omega-common-tests` umbrella.
