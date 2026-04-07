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
| **FS (`fs.h`)** | Partial | `Path`, `DirectoryIterator`, cwd/symlink/directory operations exist. File read/write, copy/move, and filtered enumeration are still missing. |
| **Net (`net.h`)** | Partial | Windows and Apple implementations exist. Non-Apple Unix still returns `NullHttpClientContext`. |
| **Multithread (`multithread.h`)** | Partial | Core threading, async/promise, semaphore, pipe, child process, and worker farm exist. Pipe and child-process ergonomics still need cleanup/documentation. |
| **JSON (`json.h`)** | Completed | Parser/serializer and object model are implemented. |
| **Format (`format.h`)** | Partial | Formatting and `LogV` exist, but structured logging and sink abstraction do not. |
| **CRT (`crt.h`)** | Partial | Minimal runtime object allocation/type helpers exist for C interop. |
| **C binding / wrapgen (`OmegaCommon.owrap`)** | Partial | A normalized test surface exists and now includes `net.h`, but the full OmegaCommon surface is not yet exposed. |
| **Regex (`regex.h`)** | Planned | Header exists but is blank. No implementation, tests, or build integration yet. |
| **Crypto (`crypto.h`)** | Planned | Header exists but is blank. No implementation, tests, or build integration yet. |
| **Third-party dependency bootstrap** | Partial | `common/AUTOMDEPS` already fetches PCRE2 and OpenSSL sources, but `common/CMakeLists.txt` does not yet build/link them into OmegaCommon. |

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
| `QueueVector` | Partial | Major `sizeof` misuse appears corrected, but the class still uses fragile manual allocation/destruction patterns and should be reviewed, fixed properly, or deprecated. |
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
| **QueueVector** | Still manually manages allocation and destruction in a way that is easy to get wrong; should either be fully repaired or deprecated in favor of `QueueHeap`/`Deque`. |
| **Regex** | `common/include/omega-common/regex.h` is empty; there is no regex API at all yet. |
| **Crypto** | `common/include/omega-common/crypto.h` is empty; there is no crypto API at all yet. |
| **Net (Unix)** | Non-Apple Unix still returns a null HTTP implementation. |
| **FS helpers** | No read/write/copy/move helpers or regex/glob-based enumeration yet. |
| **Logging** | Only `LogV` to stdout exists; no levels, sinks, or filtering. |
| **CLI / Argv** | Argument parser code remains commented out. |
| **C bindings** | Minimal CRT exists, but FS/JSON/Net/Regex/Crypto C APIs are not exposed. |
| **ChildProcess / Pipe** | Current API is usable but underspecified, and stdout piping is awkward. |
| **Codec / image** | Still not implemented. |

---

## Phase 1: Correctness, Cleanup, And Plan Alignment

Use this phase to close the remaining correctness gaps and to keep documentation aligned with the actual codebase.

### 1.1 Already Completed

- `StrRef` wide-character constructor fix is complete.
- Unix semaphore initial value fix is complete.
- `HttpResponse` ownership/status-code documentation is complete.
- The ADT additions originally planned in Phase 1.5 are mostly complete and should now be tracked as implemented rather than pending.

### 1.2 Remaining Work

- Review `QueueVector` thoroughly and choose one of:
  - fully repair it using correct array allocation/object lifetime rules, or
  - deprecate it and direct new code to `Deque` / `QueueHeap`.
- Update documentation and comments anywhere they still claim missing ADT features that are now present.
- Consider whether `common.h` should eventually include `net.h`, `regex.h`, and `crypto.h`, or whether those stay opt-in headers.

### Files

- `common/include/omega-common/utils.h`
- `common/include/omega-common/common.h`
- `common/docs/OmegaCommon-Completion-Plan.md`

---

## Phase 2: Filesystem And I/O Extensions

### 2.1 File Read/Write Helpers

- Add:
  - `Result<String, StatusCode> readFile(Path path)`
  - `StatusCode writeFile(Path path, StrRef contents)`
  - Optional binary variants for `Vector<std::uint8_t>`
- Keep text and binary helpers small and predictable; avoid hidden newline conversion.

### 2.2 Copy / Move

- Add:
  - `StatusCode copyFile(Path src, Path dest)`
  - `StatusCode moveFile(Path src, Path dest)`
  - Optional directory variants later if needed

### 2.3 Enumeration / Filtering

- Extend directory enumeration with one or both of:
  - `glob(Path dir, StrRef pattern)`
  - callback-based enumeration with filtering
- When regex lands, allow regex-based filename filtering without exposing PCRE2 details directly in FS APIs.

### Files

- `common/include/omega-common/fs.h`
- `common/src/fs.cpp`
- `common/src/win/fs-win.cpp`
- `common/src/macos/fs-cocoa.mm`
- `common/src/unix/fs-unixother.cpp`

---

## Phase 3: Networking Completion

### 3.1 HTTP On Unix

- Replace `NullHttpClientContext` on non-Apple Unix with a real implementation.
- Prefer a shared curl-backed implementation for Apple and Unix to reduce drift.

### 3.2 API Improvements

- Extend `HttpRequestDescriptor` with:
  - HTTP method
  - optional request body
  - optional headers collection instead of a single header string
- Extend `HttpResponse` with:
  - response headers
  - clearer failure semantics
  - optional convenience helpers for body-as-string

### Files

- `common/include/omega-common/net.h`
- `common/src/macos/net-curl.cpp`
- `common/src/unix/net-unix.cpp`
- `common/src/win/net-win.cpp`
- `common/CMakeLists.txt`

---

## Phase 4: Regex Library Using PCRE2

Add a lightweight OmegaCommon regex wrapper built on PCRE2. The goal is not to expose the full PCRE2 surface, but to provide a small, portable, OmegaCommon-style API for matching, searching, capture extraction, splitting, and replacement.

### 4.1 Scope

- Use PCRE2's 8-bit API to operate on `OmegaCommon::String` / `StrRef`.
- Target common needs only:
  - compile pattern
  - full match
  - search
  - capture groups
  - repeated find / find-all
  - replace / replace-all
  - split
  - escape helper
- Avoid exposing raw `pcre2_code*` or PCRE2 headers in user code if possible.

### 4.2 Proposed Public API

- `enum class RegexOption`
  - `CaseInsensitive`
  - `Multiline`
  - `DotAll`
  - `Utf`
  - `Anchored`
- `struct RegexError`
  - error code
  - compile offset
  - message string
- `struct RegexCapture`
  - start offset
  - end offset
  - matched view/string
- `struct RegexMatch`
  - full match range
  - capture list
  - convenience `group(size_t)`
- `class Regex`
  - compiled pattern object
  - move-only or shared-handle-backed
  - `static Result<Regex, RegexError> compile(StrRef pattern, unsigned options = 0)`
  - `bool matches(StrRef input) const`
  - `Optional<RegexMatch> search(StrRef input) const`
  - `Vector<RegexMatch> findAll(StrRef input) const`
  - `Result<String, RegexError> replace(StrRef input, StrRef replacement) const`
  - `Vector<String> split(StrRef input) const`
- Free helpers:
  - `String regexEscape(StrRef input)`

### 4.3 Implementation Notes

- Store compiled PCRE2 objects behind a private implementation or opaque pointer.
- Convert PCRE2 error codes into `RegexError`.
- Decide whether match results should hold copied strings or source-relative offsets plus `StrRef` views. Offsets plus views are lighter and should be preferred.
- Keep UTF behavior explicit. Do not silently assume Unicode mode unless the caller requested it.

### 4.4 Build Integration

- Wire PCRE2 into `common/CMakeLists.txt`.
- Prefer one of:
  - building from `common/deps/pcre2/code`, or
  - using a system-installed PCRE2 when available and falling back to the vendored copy
- Keep the OmegaCommon public API independent from the build strategy.

### 4.5 Tests And Docs

- Add dedicated tests covering:
  - compile errors
  - simple match/search
  - capture groups
  - UTF behavior
  - replace / split semantics
  - edge cases like empty matches
- Document ownership, match offset semantics, and supported options.

### Files

- `common/include/omega-common/regex.h`
- `common/src/regex.cpp`
- `common/CMakeLists.txt`
- `common/AUTOMDEPS`
- optional new tests such as `common/tests/regex-test.cpp`

---

## Phase 5: Lightweight Crypto Library Using OpenSSL

Add a deliberately small crypto module built on OpenSSL. This should wrap safe primitives instead of exposing broad low-level crypto machinery.

### 5.1 Scope

Initial scope should focus on safe, common building blocks:

- secure random bytes
- digest / hashing
- HMAC
- constant-time byte comparison
- optional hex encoding helpers for digest output

Encryption can be added later if there is a concrete use case, but it should not bloat the first version.

### 5.2 Proposed Public API

- `enum class DigestAlgorithm`
  - `SHA256`
  - `SHA512`
  - optional additional algorithms only when there is a clear need
- `struct CryptoError`
  - OpenSSL/library error code
  - message string
- `struct DigestResult`
  - `Vector<std::uint8_t> bytes`
  - `String hex() const`
- Free functions:
  - `Result<Vector<std::uint8_t>, CryptoError> randomBytes(size_t n)`
  - `Result<DigestResult, CryptoError> digest(DigestAlgorithm alg, ArrayRef<std::uint8_t> data)`
  - `Result<DigestResult, CryptoError> digest(DigestAlgorithm alg, StrRef text)`
  - `Result<Vector<std::uint8_t>, CryptoError> hmac(DigestAlgorithm alg, ArrayRef<std::uint8_t> key, ArrayRef<std::uint8_t> data)`
  - `bool constantTimeEquals(ArrayRef<std::uint8_t> a, ArrayRef<std::uint8_t> b)`

Optional second step if needed:

- authenticated symmetric encryption only, preferably AES-256-GCM via EVP
- explicit key/nonce/tag types to reduce misuse

### 5.3 Implementation Notes

- Use OpenSSL EVP and RAND APIs rather than handwritten algorithm-specific paths.
- Do not expose raw `EVP_MD_CTX*`, `EVP_CIPHER_CTX*`, or other OpenSSL types in the public header.
- Keep failure reporting explicit through `Result<..., CryptoError>`.
- Prefer byte-oriented APIs and then add thin `StrRef` overloads for text.
- For equality checks, use a constant-time comparison helper rather than normal `==`.

### 5.4 Build Integration

- Wire OpenSSL into `common/CMakeLists.txt`.
- Prefer either:
  - vendored source from `common/deps/openssl/code`, or
  - system OpenSSL when present with fallback to vendored source
- Keep the public OmegaCommon API independent of the exact build source.

### 5.5 Tests And Docs

- Add tests for:
  - random byte generation success
  - known-answer hash vectors
  - HMAC test vectors
  - constant-time comparison behavior
- Document clearly that this is a lightweight utility crypto layer, not a full TLS/PKI framework.
- Explicitly state what is intentionally out of scope in the first version.

### Files

- `common/include/omega-common/crypto.h`
- `common/src/crypto.cpp`
- `common/CMakeLists.txt`
- `common/AUTOMDEPS`
- optional new tests such as `common/tests/crypto-test.cpp`

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

1. Finish Phase 1 cleanup so the plan and the code agree about what is already done.
2. Implement Phase 4 Regex first, because `regex.h` is currently blank and filesystem/tooling work can benefit from it.
3. Implement Phase 5 Crypto next, starting with RNG, digests, HMAC, and constant-time compare only.
4. Complete Phase 3 networking on Unix and modernize the HTTP request/response API.
5. Add Phase 2 filesystem helpers, especially read/write and filtered enumeration.
6. Do Phase 7 logging/CLI and Phase 9 subprocess cleanup as concrete consumers need them.
7. Leave Phase 6 codec/image and Phase 8 bindings for when there is a clear consumer.

---

## Out Of Scope For The First Regex/Crypto Pass

- Exposing the full PCRE2 feature set directly.
- Re-implementing cryptographic primitives manually.
- Building a full TLS, certificate, or key-management framework.
- Requiring all OmegaCommon consumers to include regex or crypto headers through `common.h` on day one.
- Replacing all STL use across the project with OmegaCommon wrappers.

---

## Summary Table

| Phase | Focus | Status | Priority |
|-------|-------|--------|----------|
| 1 | Correctness cleanup and doc alignment | Partial | High |
| 2 | Filesystem read/write/copy/move/filtering | Planned | High |
| 3 | Unix HTTP support and request/response cleanup | Partial | High |
| 4 | PCRE2-backed regex library | Planned | High |
| 5 | OpenSSL-backed lightweight crypto library | Planned | High |
| 6 | Image/codec | Planned | Medium |
| 7 | Logging and CLI | Planned | Medium |
| 8 | C API and wrapgen surface | Partial | Medium |
| 9 | ChildProcess / Pipe cleanup | Partial | Medium |

This plan is intentionally incremental. The important update is that it now reflects completed ADT/correctness work and explicitly treats Regex and Crypto as first-class implementation phases rather than placeholder headers.
