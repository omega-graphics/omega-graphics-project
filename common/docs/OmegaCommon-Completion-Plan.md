# OmegaCommon Extension / Completion Plan

## Overview

OmegaCommon is the cross-platform base API for the omega-graphics project. It already provides a substantial ADT layer, filesystem operations, networking, JSON, formatting, multithreading primitives, and a minimal C runtime for C interop. This plan now serves two purposes:

- Record what has already been completed in the codebase.
- Define the remaining implementation work, including the currently blank `regex.h` and `crypto.h` headers.

This document treats status as one of:

- `Completed`: implemented in the current tree.
- `Partial`: some public surface exists, but important functionality is still missing or incomplete.
- `Planned`: not yet implemented.

The new library work proposed here is:

- `Regex`: a lightweight OmegaCommon wrapper over PCRE2.
- `Crypto`: a lightweight OmegaCommon wrapper over OpenSSL using safe, modern EVP/RAND APIs.

---

## Current State Snapshot

### Module Status

| Area | Status | Notes |
|------|--------|-------|
| **Core ADT (`utils.h`)** | Partial | Large ADT surface exists and several previously planned additions are now implemented; a few rough edges remain, especially `QueueVector`. |
| **FS (`fs.h`)** | Partial | `Path`, `DirectoryIterator`, cwd/symlink/directory operations exist. File read/write, copy/move, and glob filtering are now implemented. Windows `Path::absPath()` drive-letter bug fixed. |
| **Net (`net.h`)** | Completed | Full API with `HttpMethod`, request body, multi-header support, response headers, and `bodyAsString()`. Shared curl implementation for Apple and non-Apple Unix. Windows uses synchronous WinHTTP. |
| **Multithread (`multithread.h`)** | Partial | Core threading, async/promise, semaphore, pipe, child process, and worker farm exist. Pipe and child-process ergonomics still need cleanup/documentation. |
| **JSON (`json.h`)** | Completed | Parser/serializer and object model are implemented. |
| **Format (`format.h`)** | Partial | Formatting and `LogV` exist, but structured logging and sink abstraction do not. |
| **CRT (`crt.h`)** | Partial | Minimal runtime object allocation/type helpers exist for C interop. |
| **C binding / wrapgen (`OmegaCommon.owrap`)** | Partial | A normalized test surface exists and now includes `net.h`, but the full OmegaCommon surface is not yet exposed. |
| **Regex (`regex.h`)** | Completed | PCRE2-backed regex with compile, match, search, findAll, replace, split, and escape. PCRE2 built as a static library via `add_third_party`. |
| **Crypto (`crypto.h`)** | Completed | Full crypto framework: RNG, digest, HMAC, constant-time compare, AES-256-GCM, HKDF, PBKDF2, Ed25519 signatures, X.509 PKI (certificates, stores, verification), TLS (client/server contexts, streams). OpenSSL `libcrypto` + `libssl` built via `add_third_party`. |
| **Third-party dependency bootstrap** | Completed | `common/AUTOMDEPS` fetches PCRE2 and OpenSSL sources. Both are now built and linked via `CMakeLists.txt`. |

### ADT Status

| Feature | Status | Notes |
|---------|--------|-------|
| `String`, `WString`, `UString` | Completed | String aliases are present. |
| `StrRef`, `WStrRef`, `UStrRef` | Completed | Wide-character constructor bug has been fixed using `std::char_traits`. |
| `Vector`, `Array`, `ArrayRef`, `Map`, `MapVec`, `MapRef` | Completed | Core collection/view layer exists. |
| `SetVector` | Completed | Unique-by-linear-scan vector wrapper exists. |
| `Set`, `SetHash` | Completed | Added as wrappers over `std::set` and `std::unordered_set`. |
| `Span` | Completed | Mutable contiguous view exists. |
| `Deque` | Completed | Added as wrapper over `std::deque`. |
| `Stack` | Completed | Simple LIFO wrapper exists. |
| `Optional` | Completed | Alias over `std::optional`. |
| `Result<T,E>` | Completed | Lightweight success/error type now exists. |
| String helpers | Completed | `split`, `join`, `trimRef`, `trim`, `startsWith`, `endsWith`, `concat` are present. |
| Algorithm helpers | Completed | `sort`, `binarySearch`, `lowerBound`, `upperBound`, and `contains` helpers exist. |
| Hash helpers | Completed | `hashValue(StrRef)` and `hashCombine` exist. |
| `QueueHeap`, `PriorityQueueHeap` | Completed | Existing queue abstractions remain available. |
| `QueueVector` | Deprecated | Marked `[[deprecated]]` and reimplemented as a thin `Deque` wrapper. New code should use `Deque` or `QueueHeap`. |
| BitSet / RingBuffer | Planned | Still not implemented. |

### Completed Since The Original Plan

- `StrRefBase` null-terminated constructors now use `std::char_traits<CharTY>::length`.
- `Set`, `SetHash`, `Span`, `Deque`, `Stack`, and `Result<T,E>` have been added to `utils.h`.
- String helper utilities and algorithm/hash helpers have been added to `utils.h` / `utils.cpp`.
- Unix semaphore initialization now respects the constructor's `initialValue`.
- `HttpResponse` now documents allocation ownership and includes an explicit `statusCode`.

### Current Gaps And Risks

| Area | Issue |
|------|-------|
| **QueueVector** | Deprecated and replaced with `Deque`-backed wrapper. No longer a risk. |
| **Regex** | Regex is now complete with PCRE2 build integration. |
| **Crypto** | Full framework is complete: AES-GCM, HKDF, PBKDF2, Ed25519, X.509 PKI, TLS, and net integration. |
| **Net** | Networking is now complete across all three platforms. |
| **FS helpers** | `readFile`, `readBinaryFile`, `writeFile`, `writeBinaryFile`, `copyFile`, `moveFile`, and `glob` are now implemented. Regex-based filtering deferred to Phase 4. |
| **Logging** | Only `LogV` to stdout exists; no levels, sinks, or filtering. |
| **CLI / Argv** | Argument parser code remains commented out. |
| **C bindings** | Minimal CRT exists, but FS/JSON/Net/Regex/Crypto C APIs are not exposed. |
| **ChildProcess / Pipe** | Current API is usable but underspecified, and stdout piping is awkward. |
| **Codec / image** | Still not implemented. |

---

## Phase 1: Correctness, Cleanup, And Plan Alignment — Completed

### 1.1 Previously Completed

- `StrRef` wide-character constructor fix.
- Unix semaphore initial value fix.
- `HttpResponse` ownership/status-code documentation.
- ADT additions (Set, SetHash, Span, Deque, Stack, Result, string/algorithm/hash helpers).

### 1.2 Completed

- `QueueVector` deprecated: its unsafe manual memory management (VLA usage, double-destruction in destructor) has been replaced with a thin `Deque`-backed wrapper marked `[[deprecated]]`. New code should use `Deque` or `QueueHeap`.
- `common.h` decision: `net.h`, `regex.h`, and `crypto.h` remain opt-in headers. They are not included by `common.h` because regex and crypto are not yet implemented, and networking is still partial on Unix.
- Windows `Path::absPath()` drive-letter bug fixed in `fs-win.cpp`: the tokenizer was dropping the `:` from drive letters (`C:`), causing `isRelative` to be incorrectly set to `true` and `absPath()` to prepend CWD to already-absolute paths.

### Files

- `common/include/omega-common/utils.h`
- `common/include/omega-common/common.h`
- `common/src/win/fs-win.cpp`

---

## Phase 2: Filesystem And I/O Extensions — Completed

### 2.1 File Read/Write Helpers — Completed

- `Result<String, StatusCode> readFile(Path path)` — reads entire text file.
- `Result<Vector<std::uint8_t>, StatusCode> readBinaryFile(Path path)` — reads entire binary file.
- `StatusCode writeFile(Path path, StrRef contents)` — writes text, creates or overwrites.
- `StatusCode writeBinaryFile(Path path, ArrayRef<std::uint8_t> data)` — writes binary, creates or overwrites.
- All implementations are cross-platform via `std::fstream` in `fs.cpp`. No hidden newline conversion.

### 2.2 Copy / Move — Completed

- `StatusCode copyFile(Path src, Path dest)` — binary stream copy.
- `StatusCode moveFile(Path src, Path dest)` — uses `std::rename`, falls back to copy+delete across filesystems.
- Directory variants deferred to when there is a concrete consumer.

### 2.3 Enumeration / Filtering — Completed

- `Vector<Path> glob(Path dir, StrRef pattern)` — enumerates directory and filters by simple wildcard pattern (`*` and `?`).
- Regex-based filtering will be possible once Phase 4 (Regex) lands.

### Files

- `common/include/omega-common/fs.h`
- `common/src/fs.cpp`

---

## Phase 3: Networking Completion — Completed

### 3.1 HTTP On Unix — Completed

- Replaced `NullHttpClientContext` on non-Apple Unix with the shared curl implementation.
- Moved curl-backed `HttpClientContext` to `src/posix/net-curl.cpp`, used by both Apple and non-Apple Unix.
- Deleted the null stub (`src/unix/net-unix.cpp`) and the macOS-only copy (`src/macos/net-curl.cpp`).
- Fixed multiple bugs in the original curl implementation:
  - Response data is now accumulated across chunked write callbacks instead of resolving the promise per-chunk (which crashed on multi-chunk responses).
  - `curl_global_init`/`curl_global_cleanup` managed via file-scope static, not per-instance.
  - Each request creates its own `curl_easy` handle (thread-safe for concurrent calls on the same context).
  - Request headers use `curl_slist` instead of the broken `CURLOPT_HEADERDATA` misuse.
  - Memory leak of `std::promise` eliminated.

### 3.2 API Improvements — Completed

- Added `enum class HttpMethod` with `Get`, `Post`, `Put`, `Delete`, `Patch`, `Head`, `Options`.
- `HttpRequestDescriptor` now includes:
  - `HttpMethod method` (defaults to `Get`)
  - `String body` (optional request body)
  - `Vector<std::pair<String, String>> headers` (replaces the old single `header` field)
- `HttpResponse` now includes:
  - `Vector<std::uint8_t> body` (replaces raw `void *data` / `size_t size` — no more malloc ownership ambiguity)
  - `Vector<std::pair<String, String>> headers` (response headers)
  - `String bodyAsString() const` convenience method
  - `bool ok() const` (true if status code is 2xx)

### 3.3 Windows — Completed

- Adapted `net-win.cpp` to the new API shape (method, headers vector, body vector).
- Switched from async WinHTTP with a status callback to synchronous flow (matching the curl pattern).
- Replaced ICU-based wide string conversion with native `MultiByteToWideChar` / `WideCharToMultiByte`.
- Proper URL cracking, error handling, and handle cleanup.
- Response headers and body read in loops to handle large responses.

### Files

- `common/include/omega-common/net.h`
- `common/src/posix/net-curl.cpp`
- `common/src/win/net-win.cpp`
- `common/CMakeLists.txt`

---

## Phase 4: Regex Library Using PCRE2 — Completed

### 4.1 Scope — Completed

- Uses PCRE2's 8-bit API to operate on `OmegaCommon::String` / `StrRef`.
- Covers: compile, full match, search, search-from-offset, find-all, replace, split, and escape.
- No raw `pcre2_code*` or PCRE2 headers in user code. PCRE2 is hidden behind an opaque `RegexImpl` pointer.

### 4.2 Public API — Completed

- `enum class RegexOption` — bitflag enum: `CaseInsensitive`, `Multiline`, `DotAll`, `Utf`, `Anchored`.
- `struct RegexError` — code, compile offset, message string.
- `struct RegexCapture` — start/end offsets plus `StrRef` view into the source string.
- `struct RegexMatch` — full match + capture list + `group(size_t)` accessor.
- `class Regex` — move-only compiled pattern:
  - `static Result<Regex, RegexError> compile(StrRef pattern, unsigned options = 0)`
  - `bool matches(StrRef input) const`
  - `Optional<RegexMatch> search(StrRef input) const`
  - `Optional<RegexMatch> searchFrom(StrRef input, size_t startOffset) const`
  - `Vector<RegexMatch> findAll(StrRef input) const`
  - `Result<String, RegexError> replace(StrRef input, StrRef replacement) const`
  - `Vector<String> split(StrRef input) const`
- Free helper: `String regexEscape(StrRef input)`

### 4.3 Implementation Notes

- `RegexImpl` stores `pcre2_code*` and frees it on destruction.
- `compile` converts `RegexOption` flags to PCRE2 option bits and wraps compile errors in `RegexError`.
- `matches` uses `PCRE2_ANCHORED` and verifies the match covers the full input.
- `search`/`findAll` use `pcre2_match` and extract captures from the ovector.
- `findAll` advances past zero-length matches to avoid infinite loops.
- `replace` uses `pcre2_substitute` with a two-pass approach (measure then fill).
- `split` iterates matches and collects substrings between match boundaries.
- Match results hold source-relative offsets and `StrRef` views (lighter than copies). Caller must keep the input string alive while using match results.
- UTF mode is explicit — only enabled when `RegexOption::Utf` is passed.

### 4.4 Build Integration — Completed

- PCRE2 is built from the vendored source using `add_third_party` (the project's `ExternalProject_Add` wrapper).
- `PCRE2_VERSION` variable in `CMakeLists.txt` controls the source directory path — update it alongside `common/AUTOMDEPS` when upgrading.
- Only the 8-bit static library is built (`PCRE2_BUILD_PCRE2_8=ON`, tests and pcre2grep disabled).
- OmegaCommon links against the installed `libpcre2-8.a` (Unix) or `pcre2-8-static.lib` (Windows).
- PCRE2 include path is `PRIVATE` — not leaked to downstream consumers.

### 4.5 Tests

- Dedicated tests not yet added (deferred until a concrete consumer exercises the API). The API surface is ready for testing: compile errors, match/search, capture groups, UTF, replace/split, and empty-match edge cases.

### Files

- `common/include/omega-common/regex.h`
- `common/src/regex.cpp`
- `common/CMakeLists.txt`

---

## Phase 5: Lightweight Crypto Library Using OpenSSL — Completed

### 5.1 Scope — Completed

Safe, common building blocks only:

- Secure random bytes via `RAND_bytes`
- SHA-256 and SHA-512 digest via EVP
- HMAC via EVP_MAC (OpenSSL 3.0+/4.x provider API)
- Constant-time byte comparison via `CRYPTO_memcmp`
- Hex encoding on digest output

### 5.2 Public API — Completed

- `enum class DigestAlgorithm` — `SHA256`, `SHA512`.
- `struct CryptoError` — error code + message string.
- `struct DigestResult` — `Vector<std::uint8_t> bytes` + `String hex() const`.
- Free functions:
  - `Result<Vector<std::uint8_t>, CryptoError> randomBytes(size_t n)`
  - `Result<DigestResult, CryptoError> digest(DigestAlgorithm alg, ArrayRef<std::uint8_t> data)`
  - `Result<DigestResult, CryptoError> digest(DigestAlgorithm alg, StrRef text)`
  - `Result<Vector<std::uint8_t>, CryptoError> hmac(DigestAlgorithm alg, ArrayRef<std::uint8_t> key, ArrayRef<std::uint8_t> data)`
  - `bool constantTimeEquals(ArrayRef<std::uint8_t> a, ArrayRef<std::uint8_t> b)`

### 5.3 Implementation Notes

- OpenSSL EVP_MD API for digests, RAND_bytes for randomness, EVP_MAC for HMAC, CRYPTO_memcmp for constant-time compare.
- No OpenSSL types in the public header. All OpenSSL includes are in `crypto.cpp` only.
- Failure reporting uses `Result<..., CryptoError>` with OpenSSL error strings via `ERR_error_string_n`.
- Both byte-oriented (`ArrayRef<uint8_t>`) and text (`StrRef`) digest overloads are provided.

### 5.4 Build Integration — Completed

- OpenSSL is built from the vendored source at `deps/openssl/code` using `add_third_party` with `CUSTOM_PROJECT`.
- Uses `perl Configure` with `no-shared no-tests no-apps` for a minimal static build.
- Platform-specific build commands: `make` / `make install_sw` on Unix, `nmake` / `nmake install_sw` on Windows.
- Only `libcrypto` is linked — `libssl` is not needed.
- `EXPORT_STATIC_LIBS "crypto:lib/libcrypto.a:lib/libcrypto.lib"` with `EXPORT_INCLUDE_DIRS "include"`.
- OmegaCommon links `PRIVATE crypto`. OpenSSL system dependencies are linked per-platform:
  - Windows: `ws2_32`, `advapi32`, `crypt32`, `user32`
  - Linux: `dl`, `pthread`
  - macOS: no extra system libs needed

### 5.5 Tests

- Dedicated tests not yet added (deferred until a concrete consumer exercises the API). The API surface is ready for testing: random byte generation, known-answer hash/HMAC vectors, and constant-time comparison behavior.

### Files

- `common/include/omega-common/crypto.h`
- `common/src/crypto.cpp`
- `common/CMakeLists.txt`

---

## Phase 5b: Crypto Framework Extension — Completed

Extends the lightweight crypto module into a complete framework. Builds on the Phase 5 foundation (RNG, digest, HMAC, constant-time compare) by adding encryption, key derivation, digital signatures, X.509 PKI, and TLS.

### 5b.1 Secure Memory — Completed

- `secureZero(void *, size_t)` — backed by `OPENSSL_cleanse`, guaranteed not optimized away.
- `SecureAllocator<T>` — standard allocator that calls `secureZero` before deallocation.
- `SecureVector<T>` — `std::vector<T, SecureAllocator<T>>` alias. Used by `EncryptionKey`.

### 5b.2 Authenticated Symmetric Encryption — Completed

- AES-256-GCM via EVP_CIPHER API.
- Explicit types for key, nonce/IV, and authentication tag to reduce misuse.
- `EncryptionKey` — 32-byte key wrapper with `SecureVector` backing and secure zeroing on destruction. Move-only, non-copyable. Static `generate()` and `fromBytes()` constructors.
- `Nonce` — 12-byte IV wrapper with `std::array` backing. Static `generate()` and `fromBytes()`.
- `EncryptedData` — ciphertext `Vector<uint8_t>` + 16-byte GCM tag.
- `encrypt(key, nonce, plaintext, plaintextLen, aad?, aadLen?)` — AES-256-GCM encrypt with optional AAD.
- `decrypt(key, nonce, enc, aad?, aadLen?)` — AES-256-GCM decrypt with tag verification. Returns clear error on tampered data.

### 5b.3 Key Derivation — Completed

- HKDF (RFC 5869) via EVP_KDF with extract-and-expand mode. Salt and info are optional.
- PBKDF2 (RFC 8018) via EVP_KDF with configurable iteration count.
- Both use pointer+size parameters for flexibility.

### 5b.4 Digital Signatures — Completed

- Ed25519 signing and verification via EVP_PKEY.
- `SigningKey` — move-only, wraps opaque `SigningKeyImpl` (pimpl over `EVP_PKEY*`).
  - `generate()`, `fromPem()`, `toPem()`, `verifyingKey()`, `sign()`.
- `VerifyingKey` — move-only, wraps opaque `VerifyingKeyImpl`.
  - `fromPem()`, `toPem()`, `verify()` (returns `Result<bool>` — `ok(false)` for invalid signature, `err` for OpenSSL errors).
- PEM serialization for both key types, enabling interop with external tools and the TLS layer.

### 5b.5 X.509 / PKI — Completed

- `Certificate` — move-only, wraps opaque `CertificateImpl` (pimpl over `X509*`).
  - `fromPem()`, `fromDer()`, `toPem()` for loading and serialization.
  - `selfSigned(key, commonName, validDays)` — generates a self-signed X.509v3 certificate with a random 128-bit serial, Basic Constraints CA:FALSE, and EdDSA signature.
  - Inspection: `subject()`, `issuer()`, `serialNumber()`, `notBefore()`, `notAfter()` (epoch seconds), `isExpired()`.
- `CertificateStore` — move-only, wraps `X509_STORE*`.
  - `create()` for an empty store, `system()` for the OS trust store.
  - `system()` loads from common CA bundle paths on Unix/macOS, from the Windows certificate store on Windows.
  - `addCertificate()`, `verify()`, `verifyChain()` with intermediate certificates.

### 5b.6 TLS — Completed

- `TlsContext` — move-only TLS configuration wrapper (pimpl over `SSL_CTX*`).
  - `client()` — TLS 1.2+ client with system verify paths and peer verification enabled.
  - `server(certPem, keyPem)` — TLS server with any key type (RSA, ECDSA, Ed25519) loaded from PEM strings. Validates cert/key match.
  - `setVerifyPeer()`, `setCertificateStore()`, `addChainCertificate()`.
  - `connect(fd, hostname)` — TLS client handshake with SNI and hostname verification.
  - `accept(fd)` — TLS server handshake.
- `TlsStream` — move-only TLS I/O wrapper (pimpl over `SSL*`).
  - `read()`, `write()`, `shutdown()`.
  - `peerCertificate()`, `version()`, `cipherName()`.
- `SocketHandle` — `int` on Unix, `uintptr_t` on Windows.

### 5b.7 Net Layer Integration — Completed

- `HttpTlsConfig` struct with PEM-based TLS configuration: custom CA bundle, client certificate + key for mutual TLS, peer verification toggle.
- `HttpClientContext::Create(HttpTlsConfig)` overload on all platforms.
- Curl backend (Unix/macOS): stores PEM data in secure temp files with restricted permissions (0600), applies `CURLOPT_CAINFO`, `CURLOPT_SSLCERT`, `CURLOPT_SSLKEY`, and `CURLOPT_SSL_VERIFYPEER`/`CURLOPT_SSL_VERIFYHOST`. Temp files are overwritten with zeros and unlinked on destruction.
- WinHTTP backend (Windows): `verifyPeer` toggle via `WINHTTP_OPTION_SECURITY_FLAGS`. Custom trust stores and client certificates deferred to a future pass.

### 5b.8 Implementation Notes

- All OpenSSL types hidden behind pimpl (opaque `unique_ptr<Impl>` pointers). No OpenSSL headers in `crypto.h`.
- Move-only semantics on all resource types (keys, certificates, contexts, streams).
- Secure zeroing for `EncryptionKey` via `SecureAllocator`. `SigningKey` private key material managed by OpenSSL internally.
- Nonce reuse prevention is the caller's responsibility — documented in the header.
- OpenSSL `libssl` now built and linked alongside `libcrypto`.

### 5b.9 Tests

- Dedicated tests not yet added (deferred until a concrete consumer exercises the API). The API surface is ready for testing: AES-GCM known-answer vectors, HKDF/PBKDF2 test vectors, Ed25519 sign/verify round-trips, self-signed cert generation, certificate store verification, and TLS client/server handshake.

### Files

- `common/include/omega-common/crypto.h` (extended)
- `common/src/crypto.cpp` (extended)
- `common/include/omega-common/net.h` (extended with `HttpTlsConfig`)
- `common/src/posix/net-curl.cpp` (TLS config support)
- `common/src/win/net-win.cpp` (verifyPeer support)
- `common/CMakeLists.txt` (added `libssl` linkage)

---

## Phase 6: Codec And Image

### 6.1 Image Type

- Add an `Image` type with:
  - width
  - height
  - pixel format
  - owned pixel buffer

### 6.2 Decode / Encode

- Add decode/encode helpers if there is a concrete consumer.
- Keep this phase separate from regex/crypto so those blank headers are not blocked on image work.

### Files

- `common/include/omega-common/codec.h`
- `common/src/codec.cpp`
- `common/CMakeLists.txt`

---

## Phase 7: Logging And CLI

### 7.1 Logging

- Add log levels such as `Debug`, `Info`, `Warn`, `Error`.
- Add configurable sink support.
- Keep `LogV` as the simplest default path.

### 7.2 CLI / Argv

- Either finish the commented-out argument parser design or replace it with a smaller API.
- Prefer a small, predictable parser over a large framework-like layer.

### Files

- `common/include/omega-common/format.h`
- `common/src/format.cpp`
- `common/include/omega-common/utils.h`
- optional new `common/src/cli.cpp`

---

## Phase 8: C API And Bindings

### 8.1 CRT Expansion

- Add C-facing wrappers for selected OmegaCommon types when needed:
  - FS path helpers
  - JSON parsing/serialization
  - HTTP
  - Regex
  - Crypto

### 8.2 `OmegaCommon.owrap`

- Expand the wrapper surface after Regex and Crypto stabilize.
- Keep ownership explicit and binding-friendly.
- Avoid trying to expose templates or implementation details directly.

### Files

- `common/include/omega-common/crt.h`
- `common/src/crt.c`
- `common/include/OmegaCommon.owrap`

---

## Phase 9: ChildProcess And Pipe Clarity

### 9.1 ChildProcess

- Replace the single concatenated `const char *args` path with a structured argv-based API where practical.
- Provide a clean way to capture stdout/stderr as data instead of printing inside `wait()`.

### 9.2 Pipe

- Document endpoint semantics clearly.
- Confirm construction/destruction behavior on all supported platforms.

### Files

- `common/include/omega-common/multithread.h`
- `common/src/unix/multithread-unix.cpp`
- `common/src/win/multithread-win.cpp`

---

## Suggested Implementation Order

1. ~~Finish Phase 1 cleanup so the plan and the code agree about what is already done.~~ Done.
2. ~~Add Phase 2 filesystem helpers, especially read/write and filtered enumeration.~~ Done.
3. ~~Complete Phase 3 networking on Unix and modernize the HTTP request/response API.~~ Done.
4. ~~Implement Phase 4 Regex next, because `regex.h` is currently blank and filesystem/tooling work can benefit from it.~~ Done.
5. ~~Implement Phase 5 Crypto next, starting with RNG, digests, HMAC, and constant-time compare only.~~ Done.
6. ~~Implement Phase 5b Crypto Framework Extension when a consumer needs encryption, key derivation, or signing.~~ Done.
7. Do Phase 7 logging/CLI and Phase 9 subprocess cleanup as concrete consumers need them.
8. Leave Phase 6 codec/image and Phase 8 bindings for when there is a clear consumer.

---

## Out Of Scope For The First Regex/Crypto Pass

- Exposing the full PCRE2 feature set directly.
- Re-implementing cryptographic primitives manually.
- Building a full certificate authority (CA) or PKI management system (Phase 5b covers TLS and certificate verification, but not CA signing of third-party CSRs).
- Requiring all OmegaCommon consumers to include regex or crypto headers through `common.h` on day one.
- Replacing all STL use across the project with OmegaCommon wrappers.

---

## Summary Table

| Phase | Focus | Status | Priority |
|-------|-------|--------|----------|
| 1 | Correctness cleanup and doc alignment | Completed | High |
| 2 | Filesystem read/write/copy/move/filtering | Completed | High |
| 3 | Unix HTTP support and request/response cleanup | Completed | High |
| 4 | PCRE2-backed regex library | Completed | High |
| 5 | OpenSSL-backed lightweight crypto library | Completed | High |
| 5b | Crypto framework extension (AES-GCM, KDF, Ed25519, PKI, TLS) | Completed | High |
| 6 | Image/codec | Planned | Medium |
| 7 | Logging and CLI | Planned | Medium |
| 8 | C API and wrapgen surface | Partial | Medium |
| 9 | ChildProcess / Pipe cleanup | Partial | Medium |

This plan is intentionally incremental. The important update is that it now reflects completed ADT/correctness work and explicitly treats Regex and Crypto as first-class implementation phases rather than placeholder headers.
