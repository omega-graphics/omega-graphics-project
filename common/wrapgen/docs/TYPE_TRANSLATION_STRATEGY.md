# WrapGen Object-Oriented IR Translation Strategy

Date: 2026-04-26

## Goal

Define a general object-oriented intermediate representation (OO IR) for Omega
WrapGen so Python, Go, Java, Swift, Rust, and C wrappers can be generated from
the same semantic model without losing object-oriented meaning.

The important shift is this:

- The AST remains a syntax tree for `.owrap`.
- The OO IR becomes the canonical semantic API model.
- The C ABI becomes a lowering target, not the only source of truth.
- Language backends generate idiomatic proxy APIs from OO IR plus the lowered
  ABI contract.

## Current Repository Context

The current `common/wrapgen` implementation already has useful source concepts:

- `namespace`
- `class` with fields and instance methods
- `struct` with fields
- `interface` with methods
- free `func` declarations
- builtin, user, pointer, reference, const, and array type forms

The C backend currently does real generation for a subset of those concepts. It
emits opaque class handles, field accessors, instance method wrappers, C structs,
and interface vtables. Python, Go, Java, Swift, and Rust generators currently
emit skeleton files only.

The main design problem is that the current backend path flattens object-oriented
semantics too early. Once a class method becomes a free C function with a
`__self` argument, later backends need to rediscover that this was a method,
which fields are properties, what an interface means, and which types are values
versus objects.

## Research Notes

Existing wrapper systems point toward a layered design:

- SWIG uses low-level procedural wrappers beneath target-language proxy classes.
  The proxy layer restores the natural object-oriented API on top of the flat
  callable layer.
- UniFFI models a high-level component interface containing objects, records,
  methods, and the low-level FFI contract together. That separation lets target
  languages generate classes or records while sharing one ABI bridge.
- CXX separates shared structs, opaque types, and functions. That distinction is
  useful for WrapGen too: records can be passed by value when ABI-safe, while
  classes should stay opaque and be passed by reference or handle.
- wasm-bindgen is a useful reminder that binding generators should preserve rich
  surface types such as classes and strings instead of forcing every backend to
  expose only primitive handles.

For WrapGen, the practical lesson is: keep a rich semantic model first, lower it
to a procedural ABI second, then let each target generate a natural API from the
rich model.

## Proposed Pipeline

```text
.owrap source
  -> Lexer / Parser
  -> AST
  -> Semantic analysis
  -> OO IR
  -> ABI IR
  -> Target backend
      - C header/source
      - Python proxy module
      - Go package
      - Java class/JNI or FFI package
      - Swift module
      - Rust crate/module
```

The OO IR should be the stable input for every high-level target backend. The ABI
IR should describe the exact low-level callable surface required to cross the
language boundary.

## Core Design Rules

1. Preserve object identity.
   Classes and interfaces are reference-like object types in OO IR. They should
   not become raw pointers until ABI lowering.

2. Preserve value semantics.
   Structs are records. They are distinct from classes even if the C ABI later
   chooses to pass some records through pointers.

3. Preserve callable ownership.
   Methods, static methods, constructors, destructors, and free functions should
   be represented as different callable kinds.

4. Preserve field intent.
   Class fields should become properties or accessor methods in target languages.
   Struct fields should become record fields.

5. Preserve type aliases.
   Alias names should remain visible in generated target APIs while resolving to
   ABI-safe underlying types for marshaling.

6. Keep ABI lowering explicit.
   C-compatible handles, vtables, string views, array views, and error shims
   belong in ABI IR, not in the canonical OO IR.

7. Default conservatively.
   Missing ownership, nullability, lifetime, or pointer intent metadata should
   produce opaque or diagnostic behavior rather than unsafe generated APIs.

## OO IR Shape

The first implementation should use C++ classes or structs with clear ownership,
stable IDs, and deterministic ordering. Names below are conceptual, not final
header names.

### Module

`ObjectIrModule`

- source files and headers
- root namespace
- ordered declarations
- symbol table
- diagnostics metadata

### Names and Symbols

`ObjectIrSymbol`

- stable symbol ID
- simple name
- fully qualified name
- declaration kind
- parent namespace or type
- source location when available

Fully qualified names should be derived once during semantic analysis. Backends
should not reconstruct scope paths independently.

### Declarations

`ObjectIrDecl`

Common fields:

- symbol
- visibility, initially always public
- documentation text, initially empty
- attributes and annotations

Declaration kinds:

- `ObjectIrNamespace`
- `ObjectIrClass`
- `ObjectIrStruct`
- `ObjectIrInterface`
- `ObjectIrFunction`
- `ObjectIrAlias`
- future: `ObjectIrEnum`

### Classes

`ObjectIrClass`

- fields as `ObjectIrField`
- instance methods as `ObjectIrMethod`
- static methods as `ObjectIrFunction`
- constructors as `ObjectIrConstructor`
- destructor policy
- ownership model
- thread-safety metadata
- implemented interfaces, future
- base class, future

Classes are identity-bearing objects. They should lower to opaque ABI handles
unless a future backend can safely bind the native object model directly.

### Structs

`ObjectIrStruct`

- fields as `ObjectIrField`
- layout policy
- pass policy: by value, by pointer, or ABI-selected

Structs are value records. Backends should prefer target-language records,
structs, or data classes where possible.

### Interfaces

`ObjectIrInterface`

- methods as `ObjectIrMethod`
- foreign implementation support flag
- dispatch policy: vtable, callback table, or target-native interface bridge

Interfaces are contracts. The OO IR should preserve the distinction between:

- calling an interface implemented in C++
- allowing a target-language object to implement the interface and be passed back
  to C++

The first implementation can support only C++-implemented interfaces while
leaving the callback direction explicit in the model.

### Callables

`ObjectIrCallable`

- kind: free function, constructor, destructor, instance method, static method,
  property getter, property setter
- name and target name
- ordered parameters
- return type
- receiver type for methods
- receiver mutability: immutable, mutable, or unknown
- error policy
- ownership transfer policy

Parameters must be stored in source order. This should replace unordered storage
for callable parameters before broad backend work begins.

### Fields

`ObjectIrField`

- name
- type
- mutability
- property policy: direct field, getter only, getter and setter
- ownership policy for object or pointer fields

Class fields should usually lower to accessors. Struct fields can lower to ABI
fields if the field types are ABI-safe.

### Types

`ObjectIrTypeRef`

Type kinds:

- `Void`
- `Primitive`
- `String`
- `Named`
- `Alias`
- `ClassObject`
- `StructRecord`
- `InterfaceObject`
- `Pointer`
- `Reference`
- `Array`
- `Slice`
- `Optional`
- future: `Generic`, `Enum`, `FunctionPointer`

Type metadata:

- constness
- nullability
- pointer intent: opaque, input, output, inout, buffer
- ownership: borrowed, owned by caller, owned by callee, shared
- lifetime label, future

The AST `Type` object can stay simple. OO IR type resolution should turn it into
a fully resolved `ObjectIrTypeRef` that points at symbols, aliases, and builtin
type definitions.

## ABI IR Shape

The ABI IR is generated from OO IR. It describes exactly what generated C or
foreign-function code must call.

`AbiModule`

- exported functions
- exported records
- opaque handle declarations
- vtable declarations
- string and array bridge types
- ownership release functions
- error/result bridge types

`AbiFunction`

- ABI name
- source OO callable ID
- ordered ABI parameters
- ABI return type
- pre-call marshaling requirements
- post-call ownership behavior
- error behavior

`AbiType`

- primitive C type
- opaque handle
- pointer
- record
- string view
- array view
- error/result

The C backend can eventually consume ABI IR directly. Non-C backends should use
OO IR for the public surface and ABI IR for the private call layer.

## Target-Language Mapping Baseline

### Python

- Class: Python class with a private native handle.
- Struct: dataclass-like object or lightweight class.
- Interface: Python protocol or adapter class when callback support exists.
- Ownership: explicit `close()` plus context manager; finalizer as backup only.
- Calls: `ctypes` or `cffi` bridge hidden behind generated methods.

### Go

- Class: struct containing an unexported handle.
- Struct: exported Go struct.
- Interface: Go interface plus adapter when callback support exists.
- Ownership: explicit `Close()`; finalizer only as backup.
- Calls: cgo bridge hidden behind methods.

### Java

- Class: `AutoCloseable` class with a private native handle.
- Struct: plain Java class or record-like class, depending target baseline.
- Interface: Java interface plus native adapter when callback support exists.
- Ownership: deterministic `close()` and guarded native calls after close.
- Calls: JNI first, with Panama as a later option.

### Swift

- Class: `final class` with an `OpaquePointer`.
- Struct: Swift `struct`.
- Interface: Swift protocol plus adapter when callback support exists.
- Ownership: `deinit` release plus explicit close/invalidate when useful.
- Calls: generated C module imports.

### Rust

- Class: newtype around `NonNull<c_void>` or target handle.
- Struct: `#[repr(C)]` record only when layout is ABI-safe; otherwise wrapper.
- Interface: trait plus adapter when callback support exists.
- Ownership: `Drop` for owned handles; lifetimes or marker types for borrowed
  handles when the model can express them.
- Calls: unsafe FFI kept inside generated safe wrappers where possible.

## Pointer and Reference Policy

Pointers should not automatically become arrays, optionals, or owned objects.
The OO IR should preserve the raw fact first, then apply metadata.

Default rules:

- `T*` without metadata is an opaque pointer-like type.
- `const T*` without metadata is an opaque read-only pointer-like type.
- `void*` is always opaque unless an annotation gives it a target-specific role.
- `T&` is a non-null borrowed reference unless future annotations override it.
- nullable pointers map to optional target-language values.
- buffer pointers require length metadata before safe array/slice APIs are
  generated.
- owned pointers require a release function or destructor policy.

Required future metadata:

- `nullable`
- `notnull`
- `borrowed`
- `owned`
- `shared`
- `in`
- `out`
- `inout`
- `buffer(len=...)`

Until annotations exist in the DSL, defaults should be conservative and
diagnostics should explain what metadata is missing.

## Alias Policy

Aliases should be represented explicitly in OO IR:

```owrap
alias UserId = long
alias Size = unsigned long
```

Generation behavior:

- keep alias names in public target APIs
- resolve aliases for ABI marshaling
- reject recursive aliases
- preserve source alias documentation when available

Alias syntax is not currently implemented, so alias work should be planned after
the first OO IR mirror pass.

## Error and Exception Policy

C++ exceptions must not cross a C ABI boundary. OO IR should include an error
policy even if the first implementation only supports "no declared errors".

Initial policy:

- generated ABI wrappers catch C++ exceptions at the boundary only after an
  explicit exception strategy is added
- target backends map ABI errors to target-native exceptions or result types
- methods that are not error-aware should not silently swallow native failures

This can be a later thin slice, but the IR should leave room for it now.

## Base Implementation Plan with Thin Slices

Each slice should be independently testable and should preserve current behavior
unless the slice explicitly changes it.

### Slice 0: Baseline and Fixtures

Goal: freeze the current behavior before changing architecture.

Tasks:

- add representative `.owrap` fixtures for classes, structs, interfaces,
  namespaces, arrays, strings, pointers, and semantic failures
- add golden AST or parser-output tests where practical
- add generated C compile smoke tests for the current supported subset
- document known unsupported cases in test names instead of leaving them implicit

Done when:

- current tests still pass
- new fixtures describe what the current generator can and cannot do

### Slice 1: Deterministic Callable Parameters

Goal: make function and method signatures stable.

Tasks:

- replace unordered parameter storage with an ordered representation
- diagnose duplicate parameter names
- update parser, dumper, semantic checks, and C generation to use ordered params
- add regression tests proving parameter order is preserved

Done when:

- generated C signatures preserve source parameter order
- duplicate parameters fail with a clear diagnostic

### Slice 2: OO IR Skeleton

Goal: introduce OO IR without changing code generation yet.

Tasks:

- add OO IR model files under `common/wrapgen`
- create an AST-to-OOIR builder pass
- mirror existing declarations exactly: namespace, class, struct, interface,
  free function, field, method, and current type forms
- add an OO IR dump mode or internal golden-test helper

Done when:

- existing `.owrap` fixtures can be parsed and dumped as OO IR
- C generation still uses the old path and still passes existing tests

### Slice 3: Symbol and Type Resolution in OO IR

Goal: make OO IR the semantic source of truth.

Tasks:

- move or duplicate type resolution into the OO IR builder
- assign stable symbol IDs and fully qualified names
- represent builtin, named, array, pointer, reference, string, and void types as
  resolved `ObjectIrTypeRef` nodes
- keep current semantic diagnostics at least as clear as they are today

Done when:

- unknown types, namespace-as-type, and invalid `void` usage fail through OO IR
- every named type in OO IR points at a symbol or builtin definition

### Slice 4: ABI IR for Current C Features

Goal: lower OO IR into an explicit ABI model.

Tasks:

- add ABI IR model files
- lower classes to opaque handles and method/accessor functions
- lower structs to ABI records where fields are ABI-safe
- lower interfaces to vtable structs matching current behavior
- lower arrays and strings to the existing C-compatible forms
- add an ABI IR dump test

Done when:

- ABI IR can describe everything the current C backend emits
- ABI names are deterministic and derived from OO symbols

### Slice 5: Port C Backend to ABI IR

Goal: make C generation consume the new architecture.

Tasks:

- update `gen_c.cpp` to emit from ABI IR
- preserve current C output shape where possible
- keep generated header/source compile smoke tests passing
- remove backend-specific scope reconstruction that OO IR now owns

Done when:

- C output remains functionally equivalent for current fixtures
- backend code no longer mutates AST nodes, such as injecting `__self` params

### Slice 6: First Real Non-C Backend

Goal: prove OO IR can generate an idiomatic target API.

Recommended first backend: Python, because runtime iteration is fastest.

Tasks:

- generate Python class proxies for one class with fields and instance methods
- hide native handles behind generated methods/properties
- generate explicit `close()` and context-manager ownership support
- call through the ABI layer
- add a runtime smoke test against a tiny wrapped C++ fixture

Done when:

- a Python test can construct or receive a wrapped object, call a method, read a
  field, write a mutable field, and release the handle

### Slice 7: Constructors and Destructors

Goal: make class lifecycle explicit instead of incidental.

Tasks:

- add constructor/destructor representation to OO IR
- decide initial `.owrap` constructor syntax
- generate ABI create/release functions
- map ownership into Python and C output
- diagnose owned classes without release strategy

Done when:

- generated proxies have deterministic object lifetime behavior
- owned handles cannot leak silently in the happy path

### Slice 8: Struct Records and Collection Views

Goal: make value types and array-like types useful across targets.

Tasks:

- classify structs as ABI-safe or wrapper-required
- generate target-native struct/record types
- add array/slice view metadata to OO IR and ABI IR
- require buffer length metadata before generating safe list/slice APIs for raw
  pointers

Done when:

- structs round-trip through at least C and Python tests
- arrays use explicit length-bearing views rather than raw pointer guessing

### Slice 9: Interfaces and Callback Direction

Goal: preserve interface semantics across language boundaries.

Tasks:

- represent interface dispatch direction in OO IR
- support C++-implemented interfaces first
- later add target-language implementations through vtable/callback adapters
- generate target-native protocols, interfaces, or traits where appropriate

Done when:

- interface methods can be called from a generated target wrapper
- callback support is either implemented or explicitly diagnosed as unsupported

### Slice 10: Add Go, Java, Swift, and Rust Backends Incrementally

Goal: reuse the same OO IR and ABI IR without redesigning per backend.

Tasks:

- implement one backend at a time
- start with classes, structs, primitive methods, strings, and ownership
- add backend-specific tests using the same `.owrap` fixtures
- keep target language style idiomatic but generated from the same semantics

Done when:

- the same `.owrap` API produces equivalent object behavior in each completed
  target
- backend differences are documented as mapping decisions, not hidden semantic
  loss

## Recommended File Organization

Use PascalCase file names for new source files to match project guidance.

Possible new files:

- `ObjectIR.h`
- `ObjectIR.cpp`
- `ObjectIRBuilder.h`
- `ObjectIRBuilder.cpp`
- `AbiIR.h`
- `AbiIR.cpp`
- `AbiLowering.h`
- `AbiLowering.cpp`
- `ObjectIRDumper.h`
- `ObjectIRDumper.cpp`

Existing lower-case files can be migrated later as a separate cleanup. The IR
work should avoid broad file renames until the design is proven.

## Acceptance Criteria

- OO IR can represent all currently parsed object-oriented declarations.
- Backends can distinguish classes, structs, interfaces, methods, fields,
  constructors, destructors, and free functions without guessing from C names.
- C ABI lowering is deterministic and inspectable.
- Non-C target APIs expose idiomatic object-oriented wrappers, not raw flattened
  C functions.
- Pointer, ownership, and nullability behavior is either explicit or rejected
  with clear diagnostics.
- The same `.owrap` fixture can be used to test equivalent behavior across C,
  Python, Go, Java, Swift, and Rust as those backends are completed.

## Open Design Questions

- Should `.owrap` grow annotations first, or should OO IR accept synthetic
  defaults first and add annotation syntax later?

  We can add annotation syntax.

- Should Java generation start with JNI or a newer FFI option once project
  platform baselines are decided?

  We should use full JNI. We want full object bindings generated.

- Should Python use `ctypes` for low setup cost or `cffi` for stronger ABI
  modeling?

  CFFI. We want this to be the ABI to be as strict as possible.

- How much C++ exception handling should be generated by default?

  Sufficient to keep error/object handling safe.

- When inheritance is added, should the first model support only single
  inheritance, interface implementation, or both?

  Both
