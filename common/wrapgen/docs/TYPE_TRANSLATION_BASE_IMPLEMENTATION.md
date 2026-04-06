# WrapGen Type Translation Base Implementation

Date: 2026-04-05

This document turns `TYPE_TRANSLATION_STRATEGY.md` into a concrete implementation plan for `common/wrapgen`.

The goal is not to finish every backend in one step. The goal is to add a shared lowering layer that makes Python, Go, Java, Swift, and Rust backends all consume the same canonical model instead of reinterpreting the AST independently.

## Summary

The base implementation should introduce a **whole-module semantic model** and a **canonical C ABI IR** between parsing and code emission.

Recommended pipeline:

```text
.owrap source
  -> Lexer / Parser
  -> TranslationUnit AST
  -> SemanticModel (resolved names + defaults)
  -> Canonical C ABI IR
  -> Emitters:
       - C ABI header/source emitter
       - Rust binding emitter
       - Python binding emitter
       - Go / Java / Swift emitters
       - Optional JSON manifest emitter
```

This is the minimum shared architecture that lets other backends start from one truth source.

## Why This Is Needed

The current implementation has three blockers for non-C targets:

1. Generators currently consume declarations as a stream via `TreeConsumer`.
   Non-C backends need whole-module knowledge, type resolution, array-view deduplication, handle lifecycle data, and cross-reference info.

2. Function parameters are stored in `OmegaCommon::MapVec`, which is currently `std::unordered_map`.
   Parameter order is therefore unstable, which is unacceptable for FFI generation.

3. The AST type model is too close to source syntax.
   Backends need resolved categories such as "class handle", "opaque pointer", "array view", and "struct-by-value", not just `name + isPointer + isArray`.

## Scope

The base implementation should support these source features first:

- `header`
- `namespace`
- `func`
- `struct`
- `class`
- builtin scalar types
- `string`
- arrays written as `T[]`

The first shared implementation should explicitly defer or conservatively gate:

- interface callbacks for non-C languages
- raw pointer buffers without metadata
- alias syntax in `.owrap`
- exceptions
- overloads
- constructors as dedicated syntax

This keeps the first IR small enough to implement.

## Design Principles

### 1. Canonical ABI, Not Per-Backend AST Logic

Backends should not reason about C++ ABI details or source AST quirks directly.

They should consume a canonical IR where:

- classes are opaque handles
- structs are value types
- strings are explicit UTF-8 C strings
- arrays are explicit view structs
- ownership and nullability defaults are already attached

### 2. Internal IR Is The Source Of Truth

JSON output is useful for testing and debugging, but JSON should be a serialized view of the canonical IR, not the primary internal representation.

### 3. Conservative First, Richer Later

If the source language does not yet express enough metadata to safely map a construct, the base implementation should:

- lower it conservatively to opaque ABI shapes, or
- reject it for non-C backends with a clear diagnostic.

Do not guess lifecycle or buffer intent when the input is ambiguous.

## Proposed Architecture

## Phase 0: Required Groundwork

Before the new lowering layer, fix the current front-end shape.

### 0.1 Preserve parameter order

Replace function parameter storage with an ordered container.

Recommended AST change:

```cpp
struct ParamDecl {
    OmegaCommon::String name;
    Type *type;
};

struct FuncDeclNode : public DeclNode {
    OmegaCommon::String name;
    bool isStatic = false;
    bool isConstructor = false;
    OmegaCommon::Vector<ParamDecl> params;
    Type *returnType;
};
```

This should replace the current unordered `MapVec`.

### 0.2 Parse into a whole translation unit

Add a module-level result instead of only streaming declarations into a `TreeConsumer`.

Recommended shape:

```cpp
struct TranslationUnit {
    OmegaCommon::Vector<DeclNode *> decls;
};
```

Recommended parser API:

```cpp
class Parser final {
public:
    bool parse(TranslationUnit &out);
};
```

`TreeConsumer` can remain for AST dumping and temporary compatibility, but codegen should move off the streaming model.

### 0.3 Separate parsing from generation

The current `Gen` API is too tied to AST streaming.

Recommended direction:

```cpp
class Gen {
public:
    virtual void generate(const CanonicalModule &module, GenContext &ctxt) = 0;
};
```

The migration can be incremental:

- keep the old API temporarily for `parse-test`
- move real code generators to `CanonicalModule`

## Phase 1: Semantic Model

Add a semantic pass that produces resolved type information and attach defaults needed for lowering.

### 1.1 New model

Recommended new files:

- `common/wrapgen/semantic_model.h`
- `common/wrapgen/semantic_model.cpp`

Recommended core types:

```cpp
enum class SymbolKind {
    Namespace,
    Struct,
    Class,
    Interface,
    Alias
};

enum class PrimitiveKind {
    Void,
    Int,
    Float,
    Long,
    Double,
    Char,
    String
};

enum class Ownership {
    Borrowed,
    OwnedByCaller,
    OwnedByCallee,
    Shared,
    Unspecified
};

enum class Nullability {
    NonNull,
    Nullable,
    Unknown
};

enum class PointerIntent {
    Opaque,
    In,
    Out,
    InOut,
    Buffer
};
```

Resolved type shape:

```cpp
struct ResolvedType {
    enum Kind {
        Primitive,
        UserStruct,
        UserClass,
        UserInterface,
        Pointer,
        Reference,
        Array,
        Alias
    } kind;

    PrimitiveKind primitive;
    const DeclNode *decl = nullptr;
    std::shared_ptr<ResolvedType> inner;

    bool isConst = false;
    Ownership ownership = Ownership::Unspecified;
    Nullability nullability = Nullability::Unknown;
    PointerIntent pointerIntent = PointerIntent::Opaque;

    OmegaCommon::String bufferLenParam;
};
```

### 1.2 Defaults

Because the current DSL has no annotations yet, the semantic model should attach conservative defaults:

- `Class` used as a parameter: borrowed, non-null handle
- `Class` used as a return type: owned-by-caller handle
- `Struct` used as a parameter or return: by value
- `string`: borrowed UTF-8 C string
- `T[]`: array-view value type
- `T*`: opaque pointer unless metadata later says otherwise
- `const T*`: opaque pointer unless the source form is `T[]`
- `void*`: opaque nullable pointer
- reference types: borrowed, non-null, lowered like pointer in ABI

These defaults are intentionally conservative.

### 1.3 Alias support

The strategy doc includes aliases, but `.owrap` does not currently parse alias declarations.

Recommendation:

- add `Alias` to the semantic and ABI IR now
- do not block the first backend rollout on new alias syntax
- add parser support later once the shared lowering pipeline is stable

This avoids redesigning the IR later.

### 1.4 Semantic diagnostics to add

The semantic model should report backend-facing errors early:

- unsupported raw pointer usage for non-C targets
- interface usage in non-C targets before callback lowering exists
- arrays of unsupported element types
- class-by-value fields or returns that cannot be lowered safely

These should be separate from parse errors.

## Phase 2: Canonical C ABI IR

Add a lowering layer from the semantic model into a canonical ABI representation.

Recommended new files:

- `common/wrapgen/cabi_ir.h`
- `common/wrapgen/cabi_ir.cpp`
- `common/wrapgen/cabi_lower.h`
- `common/wrapgen/cabi_lower.cpp`

## Canonical IR shape

### 2.1 Module

```cpp
struct CanonicalModule {
    OmegaCommon::String name;
    OmegaCommon::Vector<OmegaCommon::String> headers;
    OmegaCommon::Vector<CAbiStruct> structs;
    OmegaCommon::Vector<CAbiHandle> handles;
    OmegaCommon::Vector<CAbiArrayView> arrayViews;
    OmegaCommon::Vector<CAbiFunction> functions;
};
```

### 2.2 ABI types

```cpp
enum class CAbiTypeKind {
    Void,
    Scalar,
    CString,
    StructValue,
    OpaqueHandle,
    OpaquePointer,
    ArrayView
};

struct CAbiScalar {
    OmegaCommon::String cSpell;
    unsigned bitWidth = 0;
    bool isSigned = true;
};

struct CAbiType {
    CAbiTypeKind kind;
    CAbiScalar scalar;
    OmegaCommon::String name;
    OmegaCommon::String targetName;
    bool nullable = false;
    bool mutableData = false;
};
```

`CAbiType` should carry both a canonical kind and enough concrete ABI detail for backend mappers.

### 2.3 Handles

```cpp
struct CAbiHandle {
    OmegaCommon::String name;
    OmegaCommon::String cTypeName;
    OmegaCommon::String releaseFunc;
    OmegaCommon::String retainFunc;
    Ownership defaultOwnership = Ownership::Borrowed;
};
```

For phase 1:

- `releaseFunc` is required for classes
- `retainFunc` can be empty until shared ownership is implemented

### 2.4 Array views

```cpp
struct CAbiArrayView {
    OmegaCommon::String name;
    CAbiType elemType;
    bool mutableData = false;
};
```

The lowerer should deduplicate array views across the module.

### 2.5 Functions

```cpp
enum class CAbiFunctionKind {
    Free,
    Method,
    Getter,
    Setter,
    Release
};

struct CAbiParam {
    OmegaCommon::String name;
    CAbiType type;
    Ownership ownership = Ownership::Borrowed;
    Nullability nullability = Nullability::Unknown;
    PointerIntent pointerIntent = PointerIntent::Opaque;
    OmegaCommon::String relatedParam;
};

struct CAbiFunction {
    CAbiFunctionKind kind;
    OmegaCommon::String publicName;
    OmegaCommon::String cSymbol;
    OmegaCommon::String cxxName;
    OmegaCommon::String ownerType;
    OmegaCommon::Vector<CAbiParam> params;
    CAbiType returnType;
};
```

## Lowering Rules

### 2.6 Source -> canonical ABI mapping

#### Scalars

- `int` -> scalar ABI type with actual C spell + width metadata
- `float` -> scalar ABI type
- `long` -> scalar ABI type with explicit recorded width
- `double` -> scalar ABI type
- `char` -> scalar ABI type

Important: the canonical ABI should record width and spelling for platform-dependent types like `long`. Other backends must not guess.

#### `string`

- lower to `CString`
- represent as UTF-8, borrowed by default
- phase 1 should treat returned strings as borrowed as well; ownership-transferring strings need later metadata

#### `struct`

- lower to `StructValue`
- fields recursively lower to supported ABI field types

#### `class`

- lower bare class types to `OpaqueHandle`
- generate one release function per class handle

This is the key design choice that lets other backends work even though the source DSL does not yet have dedicated constructor syntax.

Example:

```owrap
class Image {
  func resize(w:int, h:int) void
}

func load(path:string) Image
func draw(img:Image) void
```

Canonical ABI:

```text
OpaqueHandle ImageHandle
ImageHandle load(const char *path)
void draw(ImageHandle img)
void Image__resize(ImageHandle self, int w, int h)
void Image__release(ImageHandle self)
```

Lowering policy:

- class parameter -> borrowed handle
- class return -> owned handle
- class field getter -> owned handle or borrowed handle depending on policy; phase 1 should use copy-into-owned-handle only for copyable classes, otherwise reject until borrow metadata exists
- class field setter -> borrowed handle parameter

#### Arrays

Source `T[]` should lower to a generated array-view type, not to raw pointer parameters.

Examples:

- `int[]` -> `ArrayView<int>`
- `string[]` -> `ArrayView<CString>`
- `struct[]` -> array view of struct values

Phase 1 should reject arrays of class handles and arrays of interfaces.

#### Pointers and references

For the first shared implementation:

- `void*` -> `OpaquePointer`
- `T*` -> `OpaquePointer` unless later metadata marks it as `in`, `out`, `inout`, or `buffer`
- `const T*` -> `OpaquePointer` unless represented by source `T[]`
- `T&` -> lower like borrowed non-null pointer

This gives language backends a safe baseline without pretending to understand raw pointer APIs.

#### Interfaces

Keep interface lowering in the canonical IR, but do not require non-C emitters to support it in phase 1.

Reason:

- forward calling an interface method is easy once you already have the ABI object
- constructing interface callback trampolines from Rust/Python/Go/Java/Swift is a separate reverse-FFI problem

The base implementation should therefore:

- allow interfaces in the canonical ABI
- allow the C emitter to keep emitting vtable structs
- let non-C emitters reject interface callback generation until a later phase

## Phase 3: C ABI Emitter

Refactor the current C backend so it emits from `CanonicalModule` instead of walking the AST directly.

Recommended split:

- `common/wrapgen/emit_c_header.cpp`
- `common/wrapgen/emit_c_source.cpp`

Or keep `gen_c.cpp` but make it consume the canonical module only.

This makes the C backend the reference implementation of the shared lowering path.

The C emitter should be responsible for:

- writing typedefs for handles
- writing struct declarations
- writing array-view declarations
- writing free-function wrappers
- writing class method wrappers
- writing field getter/setter wrappers
- writing class release wrappers

## Phase 4: JSON Manifest

Add an optional serialized manifest for tests and backend bring-up.

Recommended new files:

- `common/wrapgen/manifest_json.h`
- `common/wrapgen/manifest_json.cpp`

Recommended CLI option:

```text
--emit-manifest
```

Manifest output should be a debug artifact, for example:

```json
{
  "module": "example",
  "headers": ["./myHeader.h"],
  "handles": [
    {
      "name": "Image",
      "cTypeName": "ImageHandle",
      "releaseFunc": "Image__release"
    }
  ],
  "functions": [
    {
      "kind": "Method",
      "cSymbol": "Image__resize",
      "ownerType": "Image",
      "params": [
        {"name": "self", "kind": "OpaqueHandle"},
        {"name": "w", "kind": "Scalar", "cSpell": "int"},
        {"name": "h", "kind": "Scalar", "cSpell": "int"}
      ],
      "returnType": {"kind": "Void"}
    }
  ]
}
```

This helps test the lowering layer without requiring every backend to compile immediately.

## Phase 5: First Real Backend

Implement Rust first.

Reason:

- ownership is explicit
- nullability must be modeled clearly
- handle lifecycle bugs are visible quickly
- the backend can stay thin if the canonical ABI is good

Rust phase-1 mapping:

- scalar -> Rust primitive
- `CString` -> `*const c_char` in FFI layer, `&str` or `String` in safe layer as supported
- `OpaqueHandle` -> `NonNull<c_void>` in FFI wrapper, typed owner struct in safe layer
- `StructValue` -> `#[repr(C)]` mirror struct
- `ArrayView` -> `#[repr(C)]` mirror struct, safe conversion helpers later
- `OpaquePointer` -> `*mut c_void` or `*const c_void`

Recommended Rust output shape:

- raw `extern "C"` section
- thin FFI-safe handle types
- optional safe wrapper types for owned handles with `Drop`

Example:

```rust
pub struct Image {
    raw: core::ptr::NonNull<core::ffi::c_void>,
}

impl Drop for Image {
    fn drop(&mut self) {
        unsafe { Image__release(self.raw.as_ptr()) }
    }
}
```

Once Rust works, Python can follow the same manifest with much less risk.

## File Layout Proposal

Recommended additions under `common/wrapgen`:

- `semantic_model.h`
- `semantic_model.cpp`
- `cabi_ir.h`
- `cabi_ir.cpp`
- `cabi_lower.h`
- `cabi_lower.cpp`
- `manifest_json.h`
- `manifest_json.cpp`

Recommended existing file changes:

- `parser.h`
- `parser.cpp`
- `ast.h`
- `ast.cpp`
- `wrapper_gen.h`
- `gen_c.cpp`
- `gen_rust.cpp`
- later `gen_python.cpp`, `gen_go.cpp`, `gen_java.cpp`, `gen_swift.cpp`

## CLI Changes

Recommended new driver flow in `main/main.cpp`:

```cpp
TranslationUnit tu;
if(!parser.parse(tu)) {
    return 1;
}

SemanticModel model;
if(!buildSemanticModel(tu, model, diagnostics)) {
    return 1;
}

CanonicalModule abi;
if(!lowerToCanonicalAbi(model, abi, diagnostics)) {
    return 1;
}

if(emitManifest) {
    writeManifestJson(abi, outputDir);
}

generator->generate(abi, genContext);
```

## Testing Plan

The new shared layer should be tested independently of language emitters.

### 1. Semantic-model tests

Add tests for:

- class parameter/return ownership defaults
- array element lowering
- pointer default policies
- namespace resolution
- rejection of unsupported constructs

### 2. Canonical-ABI golden tests

For each `.owrap` fixture, assert manifest contents:

- handles created for classes
- structs emitted by value
- array views deduplicated
- release functions present
- function parameter order preserved

### 3. C emitter tests

Update current integration tests to assert:

- generated class release wrappers
- class params/returns use handle types
- array-view declarations come from canonical lowering

### 4. First-backend smoke tests

For Rust:

- generated file parses
- `extern "C"` symbol names match manifest
- owned-handle wrappers call release in `Drop`

## Recommended Rollout Order

1. Fix ordered parameters in the AST.
2. Add `TranslationUnit` parser output.
3. Add semantic model with ownership/nullability defaults.
4. Add canonical ABI IR and lowerer.
5. Refactor C backend to emit from canonical ABI.
6. Add JSON manifest output and golden tests.
7. Implement Rust backend.
8. Implement Python backend.
9. Expand pointer metadata and alias syntax.
10. Revisit interfaces and callback trampolines.

## What This Plan Deliberately Does Not Solve Yet

These should be postponed until the shared lowering path is proven:

- rich annotation syntax
- callback trampolines for interfaces
- exception translation
- overload resolution
- custom memory-management policies beyond release hooks
- generic/container syntax in the source language

## Acceptance Criteria

The base implementation is complete when:

- all backends can consume `CanonicalModule` instead of raw AST
- parameter order is deterministic
- classes lower to handles with release hooks
- structs lower by value
- arrays lower to reusable view structs
- ambiguous raw pointers are either conservative opaque pointers or rejected for non-C backends
- at least one non-C backend emits real bindings from the shared IR
- the C backend uses the same lowering path as non-C backends

## Recommended First Milestone

The smallest milestone worth shipping is:

1. ordered parameters
2. `TranslationUnit`
3. `SemanticModel`
4. `CanonicalModule`
5. refactored C emitter
6. JSON manifest

At that point the project has a stable substrate for real Rust and Python backend work, and the rest of the non-C backends become mapper/emitter tasks instead of architecture work.
