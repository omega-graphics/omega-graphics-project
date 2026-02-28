# WrapGen Type Translation Strategy (Non-C Targets)

Date: 2026-02-27

## Goal
Define a practical, uniform method to translate:
- pointers (`T*`, `const T*`, `void*`)
- custom classes
- type aliases

from WrapGen AST into Python, Go, Java, Swift, and Rust bindings.

## Core Approach
Use a **2-layer model**:
1. **C ABI Layer (canonical)**: wrap all C++ entities as stable C functions + opaque handles.
2. **Language Binding Layer**: each target maps C ABI types to idiomatic target types.

This avoids re-implementing C++ ABI logic per language and keeps one truth source for ownership and lifecycle.

## Canonical Type IR
Add a resolved type model before generation:
- `Primitive(name)`
- `ClassHandle(name)`
- `Pointer(pointee, constness, nullable, ownership)`
- `Alias(name, target)`

Ownership enum (required for pointers/handles):
- `Borrowed`
- `OwnedByCaller`
- `OwnedByCallee`
- `Shared`

## Pointer Translation Rules
### Rule set
1. `T*` to primitive `T`:
- Treat as buffer/pointer API, not scalar by default.
- Require explicit annotation (`buffer`, `len`, `out`, `inout`) to generate safe binding.

2. `const T*`:
- Map as read-only view.
- Generate immutable slice/array view wrappers where possible.

3. `void*`:
- Map as opaque pointer token only.
- Disallow arithmetic or typed dereference in generated binding.

4. Nullable pointers:
- Always map to optional types (`None`/`nil`/`Option`/nullable reference).

5. Ownership-driven destructor policy:
- If binding owns pointer/handle, auto-call generated release function.

## Custom Class Translation Rules
Represent each class as an opaque handle in C ABI:
- C ABI type: `typedef struct __ns__Class* ClassHandle;`
- Generate ctor/dtor wrappers:
  - `ClassHandle class_new(...)`
  - `void class_delete(ClassHandle)`
- Generate method wrappers:
  - `Ret class_method(ClassHandle self, ...)`

Language wrappers:
- Python: class wrapping `ctypes/cffi` pointer; `__del__` or context manager for release.
- Go: struct with `unsafe.Pointer`; finalizer + explicit `Close()`.
- Java: class with `long nativeHandle`; `AutoCloseable` + `close()`.
- Swift: class/struct holding `OpaquePointer`; `deinit` release.
- Rust: struct with `NonNull<c_void>`; `Drop` for release; `Send/Sync` only if annotated safe.

## Type Alias Translation Rules
Require explicit alias table in AST/semantics:
- `alias Size = unsigned long`
- `alias UserId = long`

Generation behavior:
1. Keep alias name in target API surface (for readability and compatibility).
2. Resolve to canonical underlying ABI type for FFI calls.
3. Emit both:
- user-facing alias (e.g., `type UserId = i64` in Rust)
- internal marshaling to ABI primitive.

## Language Mapping Baseline
- Python:
  - primitives -> Python scalars
  - pointers/handles -> capsule/wrapper class
- Go:
  - primitives -> Go primitives
  - handles -> `uintptr`/`unsafe.Pointer` hidden behind typed wrapper
- Java:
  - primitives -> Java primitives
  - handles -> `long` hidden in class
- Swift:
  - primitives -> Swift primitives
  - handles -> `OpaquePointer`
- Rust:
  - primitives -> `i32/f32/i64/f64` etc.
  - handles -> `NonNull<c_void>` newtype

## Required Metadata Extensions
To make this robust, add wrapper annotations (or equivalent semantic metadata):
- pointer intent: `in`, `out`, `inout`, `buffer(len=...)`
- nullability
- ownership transfer
- thread-safety hints for handles (`sendable`, `sync`)

Without these, pointer translation should default to conservative opaque behavior.

## Suggested Implementation Order
1. Build semantic pass for alias resolution + ownership/nullability defaults.
2. Generate complete C ABI metadata manifest (JSON or internal IR dump).
3. Implement Rust or Python backend first end-to-end with full pointer + class lifecycle.
4. Reuse same manifest-driven mapper for Go/Java/Swift.

## Acceptance Criteria
- Same `.owrap` API generates equivalent behavior across all targets.
- No backend directly depends on C++ ABI details.
- Pointer ownership bugs are prevented by generated lifecycle hooks.
- Alias names preserved in target surface while marshaling remains ABI-correct.
