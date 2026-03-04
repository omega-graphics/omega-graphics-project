# OmegaCommon Extension / Completion Plan

## Overview

OmegaCommon is the cross-platform base API for the omega-graphics project. It provides an **ADT library** (strings, containers, references, queues, smart pointers), filesystem operations, networking (HTTP), JSON, formatting, multithreading (threads, mutexes, async/promise, semaphore, pipes, child processes), and a minimal C runtime (CRT) for C interop. This document proposes an implementation plan to extend and complete the library, including evolving the ADT layer into a full-featured ADT lib.

---

## ADT Library: Current State and Full ADT Lib Vision

### Current ADT Inventory

| Category | Type | Notes |
|----------|------|-------|
| **Strings** | `String`, `WString`, `UString` | `std::string` / `u16string` / `u32string` aliases |
| **String refs** | `StrRef`, `WStrRef`, `UStrRef` | Immutable view; `StrRef` has `strlen` bug for wide chars |
| **Small string** | `SmallHeapString`, `SmallWHeapString`, `SmallUHeapString` | HeapString with small buffer (10 chars) |
| **Sequences** | `Vector<T>`, `Array<T,N>` | `std::vector` / `std::array` aliases |
| **Sequence refs** | `ArrayRef<T>`, `ContainerRefBase<T>` | Immutable view over contiguous sequence; `makeArrayRef(beg,end)` |
| **Sets** | `SetVector<T>` | Vector with uniqueness (linear scan; no ordering guarantee) |
| **Maps** | `Map<K,V>`, `MapVec<K,V>` | `std::map` (ordered) / `std::unordered_map` aliases |
| **Map refs** | `MapRef<K,V>` | Const view over map |
| **Queues** | `QueueHeap<T>`, `PriorityQueueHeap<T,Cmp>` | FIFO with resize; priority queue by comparison |
| **Queues (broken)** | `QueueVector<T>` | Buggy (wrong `sizeof` usage); FIFO with index access |
| **Optional / RC** | `Optional<T>`, `ARC<T>`, `ARCAny<T>`, `RuntimeObject` | `std::optional`; ref-counted pointer |
| **Handles** | `SharedHandle<T>`, `UniqueHandle<T>`, `make`, `construct` | `shared_ptr` / `unique_ptr` wrappers |
| **Allocators** | `HeapAllocator<T,N>` | Fixed initial size, grows; used by HeapString/HeapVector |

### Gaps for a Full ADT Library

- **Set (hash / ordered)**: No dedicated `Set<T>` (unique keys, no value). Only `SetVector` (vector + uniqueness).
- **Deque**: No double-ended queue (push/pop front and back in O(1) amortized).
- **Stack**: No LIFO abstraction; could be a thin wrapper over `Vector` or `Deque`.
- **Mutable span**: `ArrayRef` is const-only; no `Span<T>` or mutable view for in-place algorithms.
- **Result / Either**: No `Result<T,E>` or `Either<L,R>` for explicit error handling (vs. `Optional` or out-params).
- **String utilities**: No `join`, `split`, `trim`, `startsWith`/`endsWith` on `StrRef`/`String`; no string builder.
- **Algorithms**: No generic `sort`, `binarySearch`, `lowerBound` on `ArrayRef`/`Vector`; no `contains` for sets/maps.
- **Hashing**: No `Hash` trait or `hashCombine` for use with `MapVec`/future `Set`; no standard hash for `StrRef`.
- **Iteration**: No range/iterator helpers (e.g. `enumerate`, `zip`) beyond STL; optional.
- **BitSet**: No fixed or dynamic bit set for flags / small integer sets.
- **Ring buffer**: No fixed-capacity circular buffer (useful for streaming / queues with bounded memory).

### ADT Library Completion Goals

- **Correctness**: Fix `StrRef` wide-char and `QueueVector` (or remove); make existing types robust.
- **Completeness**: Add Set, Deque, Stack, mutable Span, Result/Either, string helpers, and algorithm helpers so that OmegaCommon can serve as the single ADT dependency for omega-graphics.
- **Consistency**: Uniform naming (e.g. `push`/`pop`/`front`/`back` where applicable), optional custom allocator support later.
- **Documentation**: Clear ownership (view vs. owned), complexity, and thread-safety notes for each type.

---

## Current State

### What Exists and Works

| Area | Status | Notes |
|------|--------|-------|
| **Core types / ADT (utils.h)** | Implemented | See [ADT Library](#adt-library-current-state-and-full-adt-lib-vision): `String`, `StrRef`, `Vector`, `Map`, `MapVec`, `ArrayRef`, `SetVector`, `QueueHeap`, `PriorityQueueHeap`, `ARC`, `Optional`, `SharedHandle`/`UniqueHandle`; no `Set`/`Deque`/`Stack`/`Span`/`Result` yet. |
| **FS (fs.h)** | Implemented | `Path` (parse, append/concat, dir/filename/ext, `absPath`, `exists`, `isFile`, `isDirectory`, `isSymLink`, `followSymlink`), `DirectoryIterator`, `changeCWD`, `createSymLink`, `createDirectory`, `deleteDirectory`; platform impls: Win, Cocoa, Unix |
| **Net (net.h)** | Partial | `HttpClientContext::Create()`, `makeRequest()`; CURL on Apple, Win impl on Windows; **Unix (non-Apple)** uses `NullHttpClientContext` (no-op) |
| **Multithread (multithread.h)** | Implemented | `Thread`, `Mutex`, `Async`/`Promise`, `Semaphore`, `Pipe`, `ChildProcess`, `WorkerFarm`; platform impls for Win and Unix/Apple |
| **JSON (json.h)** | Implemented | `JSON` (string/array/map/number/boolean), `parse`/`serialize`, `JSONConvertible`; lexer/parser in json.cpp |
| **Format (format.h)** | Implemented | `FormatProvider`, `fmtString`, `Formatter` with `@` placeholders, `LogV` |
| **CRT (crt.h)** | Minimal | `OmegaRTObject`, `omega_common_alloc`/`get_data`/`exists`/`typecheck`/`free` in crt.c |
| **C binding (owrap)** | Minimal | `OmegaCommon.owrap` only includes `common.h`; wrapgen exists for multi-language bindings |
| **Utilities** | Implemented | `findProgramInPath()` in utils.cpp |

### What Is Missing or Broken

| Area | Issue |
|------|--------|
| **StrRef** | `StrRefBase(const CharTY *c_str)` uses `strlen(c_str)` — invalid for `char16_t`/`char32_t`; should use `std::char_traits<CharTY>::length` or overloads. |
| **QueueVector** | Uses `len * sizeof(Ty)` and `sizeof(Ty)` for element counts and pointer arithmetic in multiple places; should use `len` (number of elements) and `_data + len`. Class is buggy and likely unused; consider deprecating or fixing. |
| **codec.h** | `Image` struct is empty; no image encode/decode (e.g. PNG/JPEG), no width/height/format/pixel buffer. |
| **Argv (CLI)** | Entire `Argv` namespace in utils.h is commented out — no command-line argument parsing. |
| **C bindings** | CRT covers only generic object alloc/free/typecheck. No C API for FS, JSON, Net, or Format. `OmegaCommon.owrap` does not expose these. |
| **Net (Unix)** | On non-Apple Unix, `HttpClientContext::Create()` returns a null implementation; no real HTTP. |
| **FS helpers** | No `readFile`/`writeFile`, `copyFile`/`moveFile`, or glob/regex-based enumeration. |
| **Logging** | `LogV` only writes to stdout; no log levels, file/sink abstraction, or filtering. |
| **Semaphore (Unix)** | `sem_init(&sem, 0, 0)` uses 0 for initial value; should use `initialValue` parameter. |
| **Pipe (Unix)** | `pipe(pipe_fd)` is used but `Pipe` constructor is private; `Pipe` is only used as friend by `ChildProcess`. API and lifecycle are unclear. |
| **ChildProcess** | `OpenWithStdoutPipe` takes a single `const char *args` string (e.g. concatenated); no structured argv. `wait()` on Unix with pipe reads and prints stdout instead of exposing it. |
| **HttpResponse** | `void *data` with no documented ownership; caller may not know whether to free. |
| **ADT gaps** | No `Set`/`SetHash`, `Deque`, `Stack`, mutable `Span`, `Result`/`Either`; no string helpers (split/join/trim/startsWith); no algorithm helpers (sort, binarySearch, contains); no hashing for `StrRef`. See [ADT Library](#adt-library-current-state-and-full-adt-lib-vision). |

---

## Phase 1: Correctness and Safety

Stabilize existing APIs and fix known bugs before adding features.

### 1.1 StrRef wide-character constructors

- **Problem**: `StrRefBase<CharTY>(const CharTY *c_str)` uses `strlen()`, which is only valid for `char`.
- **Change**: Use `std::char_traits<CharTY>::length(c_str)` for the two `c_str` constructors, or provide explicit overloads for `char`, `char16_t`, `char32_t` with the correct length function.
- **Files**: `common/include/omega-common/utils.h`.

### 1.2 QueueVector fix or deprecation

- **Problem**: Multiple uses of `len * sizeof(Ty)` and `sizeof(Ty)` where element count or pointer offset is intended (e.g. `end()` returns `_data + (len * sizeof(Ty))`).
- **Options**:  
  - **Fix**: Use `_data + len` for `end()`, and fix all move/copy loops to use element counts and `_data + idx` (not `sizeof(Ty)*idx`).  
  - **Deprecate**: If unused, mark deprecated and add a short comment; prefer `QueueHeap` or `std::queue` for new code.
- **Files**: `common/include/omega-common/utils.h`.

### 1.3 Semaphore initial value (Unix)

- **Problem**: `sem_init(&sem, 0, 0)` ignores `initialValue`; should be `sem_init(&sem, 0, initialValue)`.
- **Files**: `common/src/multithread-unix.cpp`.

### 1.4 HttpResponse ownership

- **Change**: Document that `HttpResponse::data` is allocated with `malloc` and must be freed by the caller (or provide a small helper that wraps response + free). Optionally add a `StatusCode` or success flag to `HttpResponse`.
- **Files**: `common/include/omega-common/net.h`, and all HTTP implementations (e.g. net-curl.cpp, net-win.cpp).

---

## Phase 1.5: ADT Library Completion

Expand the ADT layer so OmegaCommon qualifies as a full ADT library: fix bugs, add missing container and value types, string helpers, and algorithm helpers.

### 1.5.1 Set types

- **Add**:
  - `Set<T>` — unique elements, ordered (backed by sorted structure or `std::set`); `insert`, `erase`, `contains`, iteration.
  - `SetHash<T>` or `HashSet<T>` — unique elements, O(1) lookup (backed by `std::unordered_set`); same operations.
- **Optional**: `SetVector` remains as “ordered insertion, uniqueness, vector iteration”; document as “set with insertion order and vector API”. Consider `contains(T)` and `erase(T)` for consistency.
- **Files**: `common/include/omega-common/utils.h` (or new `common/include/omega-common/adt.h` if splitting).

### 1.5.2 Deque and Stack

- **Add**:
  - `Deque<T>` — double-ended queue: `pushFront`/`pushBack`, `popFront`/`popBack`, `front`/`back`, `size`/`empty`. Implement via chunked buffer or wrap `std::deque`.
  - `Stack<T>` — LIFO: `push`, `pop`, `top`, `size`/`empty`. Can wrap `Vector<T>` or `Deque<T>`.
- **Files**: `common/include/omega-common/utils.h` (or `adt.h`).

### 1.5.3 Mutable Span

- **Add**: `Span<T>` — mutable view over contiguous memory (like `ArrayRef<T>` but non-const). Construct from `Vector<T>&`, `T*`+length, iterators. Support `operator[]`, `size`, `begin`/`end`, and optional `subspan(offset, count)`.
- **Note**: Keep `ArrayRef<T>` as the const view; `Span<T>` for in-place algorithms and output buffers.
- **Files**: `common/include/omega-common/utils.h` (or `adt.h`).

### 1.5.4 Result / Either

- **Add**: `Result<T, E>` — holds either a value `T` or an error `E`; `isOk()`/`isErr()`, `value()`/`error()`, `valueOr(default)`, and optional `map`/`andThen`-style helpers. Alternative: `Either<L, R>` if a single sum type is preferred.
- **Use case**: Return types for FS/Net APIs instead of only `StatusCode` + out-params.
- **Files**: `common/include/omega-common/utils.h` (or `adt.h`).

### 1.5.5 String helpers

- **Add** (on `StrRef` and/or free functions taking `StrRef`):
  - `split(StrRef s, CharTY delim)` → `Vector<String>` (or split on `StrRef` delimiter).
  - `join(ArrayRef<StrRef> parts, StrRef sep)` → `String`.
  - `trim(StrRef s)` → `StrRef` (view of trimmed range) or `String`.
  - `startsWith(StrRef s, StrRef prefix)`, `endsWith(StrRef s, StrRef suffix)` → `bool`.
  - Optional: `StringBuilder` or `concat(ArrayRef<StrRef>)` for efficient multi-part concatenation.
- **Files**: `common/include/omega-common/utils.h`, `common/src/utils.cpp`.

### 1.5.6 Algorithm helpers

- **Add**: Non-member functions operating on `ArrayRef<T>` / `Span<T>` / `Vector<T>`:
  - `sort(ArrayRef<T>)`, `sort(ArrayRef<T>, Compare)` — in-place sort.
  - `binarySearch(ArrayRef<T>, const T&)` → `Optional<size_t>`; `lowerBound`/`upperBound` → index or iterator.
  - `contains(const Set/Map/SetVector&, key)` — consistent `contains` API where applicable.
- **Files**: `common/include/omega-common/utils.h` (or `adt.h`), possibly `common/src/adt.cpp` if non-inline.

### 1.5.7 Hashing

- **Add**: `hashValue(StrRef)`, `hashValue(T)` for common types (delegate to `std::hash` where possible). Optional: `hashCombine(size_t seed, T...)` for composite keys. Ensures `StrRef` and OmegaCommon types can be used as keys in `MapVec`/`SetHash` with stable hashing.
- **Files**: `common/include/omega-common/utils.h`, `common/src/utils.cpp` if non-inline.

### 1.5.8 BitSet and RingBuffer (optional)

- **BitSet**: Fixed-size or dynamic bit set: `BitSet<N>` or `BitSet` (dynamic); `set(i)`, `reset(i)`, `test(i)`, `count()`, iteration over set bits.
- **RingBuffer**: `RingBuffer<T, N>` (fixed capacity) or `RingBuffer<T>` (dynamic); push/pop front/back, overwrite semantics when full. Useful for streams and bounded queues.
- **Files**: `common/include/omega-common/utils.h` (or `adt.h`). Lower priority; add when needed.

### 1.5.9 QueueVector resolution

- **Action**: Either fix `QueueVector` (Phase 1.2) or remove/deprecate it in favor of `QueueHeap` + `Deque`. If kept, add to ADT docs with correct complexity and semantics.

---

## Phase 2: Filesystem and I/O Extensions

### 2.1 File read/write helpers

- **Add**:  
  - `OmegaCommon::FS::readFile(Path path) -> Optional<String>` (or `StatusCode readFile(Path, String &out)`).  
  - `StatusCode writeFile(Path path, StrRef contents)`.  
  - Optionally: binary variants `readFileBinary` / `writeFileBinary` returning/accepting `std::vector<uint8_t>` or `ArrayRef<uint8_t>`.
- **Files**: `common/include/omega-common/fs.h`, `common/src/fs.cpp` (+ platform files if needed).

### 2.2 Copy and move

- **Add**:  
  - `StatusCode copyFile(Path src, Path dest)` (and optionally `copyDirectory`).  
  - `StatusCode moveFile(Path src, Path dest)` (and optionally `moveDirectory`).  
- Implement via platform APIs (CopyFile/MoveFile on Windows, copyfile()/rename on macOS, copy_file/rename on C++17/posix).
- **Files**: `common/include/omega-common/fs.h`, `common/src/fs.cpp`, `fs-win.cpp`, `fs-cocoa.mm`, `fs-unixother.cpp`.

### 2.3 Directory enumeration improvements

- **Current**: `DirectoryIterator` exists; behavior and completeness on all platforms should be verified.
- **Add (optional)**:  
  - `FS::glob(Path dir, StrRef pattern)` or `enumerate(Path dir, std::function<bool(Path)>)` to allow filtering (e.g. by extension or regex).  
  - Explicit documentation of iteration order and symlink behavior.

---

## Phase 3: Networking Completion

### 3.1 HTTP on Unix (non-Apple)

- **Problem**: On Unix, `HttpClientContext::Create()` returns `NullHttpClientContext`; no real HTTP.
- **Options**:  
  - Use libcurl on Unix as well (add `curl` dependency and a `net-curl.cpp` path for Unix, or a single net-curl.cpp used for both Apple and Unix).  
  - Or implement a small request path with another library (e.g. libcurl is the most portable).
- **Files**: `common/CMakeLists.txt`, `common/src/net-unix.cpp` (or new/refactored net-curl.cpp), `net.h`.

### 3.2 HTTP API improvements

- **Add**:  
  - HTTP method (GET/POST/etc.) and optional body in `HttpRequestDescriptor`.  
  - Response status code and headers (e.g. `int statusCode`, `Map<String,String> headers`) in `HttpResponse`.  
  - Clear ownership and error semantics (see Phase 1.4).

---

## Phase 4: Codec and Image

### 4.1 Image type and codec.h

- **Add**:  
  - `Image` with at least: width, height, pixel format (e.g. enum: RGBA8, RGB8, Gray8), and a pixel buffer (`Vector<uint8_t>` or similar).  
  - Optional: `Image decodeImage(StrRef path)` / `decodeImage(ArrayRef<uint8_t> buffer)` and `encodeImage(Path path, const Image &)` using a chosen backend (e.g. stb_image/stb_image_write, or libpng/libjpeg if already in tree).  
- **Files**: `common/include/omega-common/codec.h`, new `common/src/codec.cpp` (and possibly optional backend files or CMake options for dependencies).

---

## Phase 5: Logging and CLI

### 5.1 Logging

- **Add**:  
  - Log level (e.g. Debug, Info, Warn, Error).  
  - `Log(level, fmt, ...)` and optionally a small `Logger` class with configurable sink (ostream, file, or callback).  
  - Keep `LogV` as a simple default (e.g. Info level to stdout).
- **Files**: `common/include/omega-common/format.h` (or new `log.h`), `common/src/format.cpp` or new `log.cpp`.

### 5.2 Argument parser (Argv)

- **Current**: Entire `Argv` namespace commented out in utils.h.
- **Option A**: Uncomment and complete the design (flags, positional args, help generation, type parsers for bool/String/Vector<String>).  
- **Option B**: Introduce a minimal API: e.g. `parseArgs(int argc, char **argv, Map<String,String> &flags, Vector<String> &positional)` and optional helpers for common types.  
- **Files**: `common/include/omega-common/utils.h` (and possibly a new `argv.h` or `cli.h`), plus a small implementation file.

---

## Phase 6: C API and Bindings

### 6.1 Extend CRT for common types

- **Add** (as needed for C consumers):  
  - Opaque handles for `FS::Path` (e.g. `OmegaFSPath*`), create/destroy and get string.  
  - JSON: create from string, get type, get string/number/array/map, serialize.  
  - Optional: HTTP request/response C API (create context, make request, read response, free).  
- **Files**: `common/include/omega-common/crt.h`, `common/src/crt.c` (and possibly C++ bridge in crt.cpp that calls into OmegaCommon C++).

### 6.2 OmegaCommon.owrap and wrapgen

- **Extend** `OmegaCommon.owrap` to declare the C++ APIs that should be exposed to C (and thus to wrapgen’s C ABI layer).  
- Align with wrapgen’s type translation strategy (opaque handles, explicit ownership).  
- **Files**: `common/include/OmegaCommon.owrap`, wrapgen targets as needed.

---

## Phase 7: Child Process and Pipe Clarity

### 7.1 ChildProcess API

- **Improve**:  
  - `OpenWithStdoutPipe`: consider taking `Vector<StrRef>` or `ArrayRef<const char*>` for argv instead of a single concatenated string, to avoid quoting issues.  
  - Provide a way to read stdout into a buffer or string instead of printing in `wait()`.  
  - Document that on some platforms stdout is only available after the process exits (e.g. current popen behavior).
- **Files**: `common/include/omega-common/multithread.h`, `common/src/multithread-unix.cpp`, `multithread-win.cpp`.

### 7.2 Pipe

- **Document**: Pipe is for use with ChildProcess; clarify which side is parent/child and how to use read/write. Fix or document the Unix `Pipe` constructor (e.g. `pipe()` in constructor) and destructor (close fds).

---

## Implementation Order (Suggested)

1. **Phase 1** (correctness): 1.1 StrRef, 1.2 QueueVector, 1.3 Semaphore, 1.4 HttpResponse.  
2. **Phase 1.5** (ADT lib): 1.5.1 Set types, 1.5.2 Deque/Stack, 1.5.3 Span, 1.5.4 Result, 1.5.5 string helpers, 1.5.6 algorithm helpers, 1.5.7 hashing; then 1.5.8 BitSet/RingBuffer and 1.5.9 QueueVector resolution as needed.  
3. **Phase 2** (FS): 2.1 read/write, 2.2 copy/move, 2.3 directory enumeration if needed.  
4. **Phase 3** (Net): 3.1 HTTP on Unix, 3.2 HTTP API improvements.  
5. **Phase 5.1** (logging): small addition so other modules can log consistently.  
6. **Phase 4** (Image/codec) and **Phase 5.2** (Argv): as needed by apps.  
7. **Phase 6** (C API / owrap): when C or generated bindings are required.  
8. **Phase 7** (ChildProcess/Pipe): when subprocess use cases are critical.

---

## Out of Scope (for this plan)

- **omega-wrapgen** and **omega-ebin**: Remain separate tools; this plan focuses on the OmegaCommon library API and implementation.  
- **C++ standard**: No specific bump (e.g. C++17/20) mandated here; assume current project standard.  
- **Build**: OmegaCommon is not built standalone by default; CMake and dependency changes (e.g. curl on Unix) are noted only where they affect the plan.  
- **ADT**: The plan does not require replacing all `std::` usage in the codebase with OmegaCommon types; the goal is to offer a complete, consistent ADT lib for new code and gradual adoption.

---

## Summary Table

| Phase | Focus | Priority |
|-------|--------|----------|
| 1 | StrRef, QueueVector, Semaphore, HttpResponse correctness | High |
| **1.5** | **ADT lib: Set, Deque, Stack, Span, Result, string/algo/hash helpers** | **High** |
| 2 | readFile/writeFile, copyFile/moveFile, directory | High |
| 3 | HTTP on Unix, request/response API | Medium |
| 4 | Image type and decode/encode | Medium |
| 5 | Logging levels/sinks, Argv/CLI | Medium |
| 6 | C API and owrap | Lower |
| 7 | ChildProcess/Pipe API and docs | Lower |

This plan can be implemented incrementally; each phase can be split into smaller tasks and merged as needed.
