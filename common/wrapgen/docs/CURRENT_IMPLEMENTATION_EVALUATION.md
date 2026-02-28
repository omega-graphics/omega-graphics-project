# Omega WrapGen: Current Parser and Generation Capability Evaluation

Date: 2026-02-27

## Scope
This document evaluates the current implementation in `common/wrapgen` for:
- Lexing and parsing capabilities
- AST and semantic model maturity
- Code generation status by target language
- Test coverage and practical readiness

## Executive Summary
The wrapgen pipeline is functional for a narrow subset of the DSL and can currently produce meaningful C wrapper output. The parser handles core declarations (`header`, `class`, `func`, `namespace`) and builds an AST consumed by generators. Multi-language mode selection now works in the CLI, but non-C generators are currently skeleton emitters rather than real wrappers.

Current maturity:
- Parsing: Early functional
- C generation: Partial but usable for simple class-method wrapping
- Python/Go/Java/Swift/Rust generation: Skeleton only
- Semantic validation: Stubbed
- Test suite: Basic integration coverage for mode/output smoke tests

## Parser Capability Evaluation

### What Works
- Tokenization of identifiers, keywords, braces, parentheses, colons, commas, `*`, `&`, string literals, and line comments.
- Parsing of declaration-level constructs:
  - `header "..."`
  - `class Name { ... }`
  - `func name(params...) return_type`
  - `namespace Name { ... }`
- Construction of AST nodes with scope association (`TreeScope`) and type objects (`Type`).
- Parsing loop can stream top-level declarations to a `TreeConsumer`.

### Current Grammar Envelope
Supported keywords in lexer/parser:
- `class`
- `func`
- `const`
- `interface` (token recognized but parser handling is not implemented)
- `namespace`
- `header`

Type parsing currently supports:
- Built-ins: `void`, `int`
- Custom identifiers
- Pointer/reference qualifiers via `*` and `&`
- Optional `const` prefix

### Key Gaps and Risks
- Semantic analysis is effectively unimplemented.
  - `TreeSemantics` exists but does not enforce symbol checks, duplicate declarations, or type rules.
- Parser internals use global static builder/semantics state.
  - This is not reentrant and is unsafe for concurrent parser instances.
- Error handling reports generic messages without line/column tracking.
  - Diagnostics are useful for failures but low precision for debugging larger files.
- Memory ownership is mostly raw-pointer based in AST and diagnostics.
  - No lifecycle cleanup model is in place, which risks leaks in long-running use.
- Lexer is minimal and not robust against broader syntax cases.
  - No block comment support despite token constant declaration.
  - No escape handling in string literals.
  - Only alphanumeric identifiers; underscore handling is limited.

## Generation Capability Evaluation

### CLI Mode Selection
`omega-wrapgen` now supports mode flags and input/output handling:
- `--cc`
- `--python`
- `--go`
- `--java`
- `--swift`
- `--rust`

It validates unknown flags, missing input, and missing output-dir arguments.

### C Generator (`gen_c.cpp`)
Status: Partial implementation with real AST consumption.

Implemented behavior:
- Emits `<name>.h` and `<name>.cpp`
- Emits header guards and generated-file comment
- Converts `header` declarations into includes in generated source
- Wraps class instance methods by:
  - Emitting opaque C typedefs for C++ class-backed structs
  - Emitting `extern "C"` C-callable wrapper functions
  - Injecting a `__self` argument for instance method dispatch

Known limitations:
- Namespace/interface generation is mostly no-op.
- Static methods are parsed but not emitted.
- Parameter storage uses `MapVec` (unordered), so argument order is not guaranteed.
- `header` include path rewriting is heuristic and may fail for complex layouts.
- No overload resolution strategy, ownership policy, or exception boundary handling.

### Python / Go / Java / Swift / Rust Generators
Status: Skeleton output only.

Implemented behavior:
- Each mode creates exactly one language-specific output file.
- Each file includes generated-file marker text.
- Each file tracks declaration count (`DECL_COUNT` or equivalent).

Not yet implemented:
- No translation from AST declarations to callable wrappers/bindings.
- No runtime glue, FFI layer, package/module structure, or type marshaling.

## Test Coverage Evaluation

### Existing Tests
- `parse-test`: AST dump smoke test for parser consumption.
- `lex-test`: token stream printing utility (not currently wired as active CMake target).
- `wrapgen-tests` custom target runs Python integration tests (`common/wrapgen/tests/run_tests.py`).

### What the Current Suite Validates
- All mode flags execute successfully.
- Each mode generates expected output file names.
- Generated files are non-empty.
- Unknown mode handling returns non-zero and prints an error.

### What the Current Suite Does Not Validate
- AST semantic correctness beyond basic parsing.
- C output compileability against real source headers.
- Correct function signatures, parameter ordering, and method binding behavior.
- Any real wrapper behavior for Python/Go/Java/Swift/Rust.

## Practical Readiness Assessment

Current readiness by target:
- C wrappers for simple class methods: Experimental/early-adopter
- Multi-language wrapper generation: Not production-ready
- Parser as a front-end for richer generators: Promising but incomplete

Recommended immediate confidence level:
- Safe for internal prototyping and format iteration.
- Not safe yet for production wrapper generation workflows.

## Priority Recommendations

1. Parser and AST reliability
- Replace global static parser state with instance-owned objects.
- Add source location metadata to tokens/AST nodes.
- Implement deterministic parameter ordering (switch `MapVec` to ordered structure).

2. Semantic layer
- Implement symbol table population and duplicate detection.
- Validate return/parameter types and scope references.
- Add structured diagnostic codes with context.

3. C backend hardening
- Add static method and namespace emission.
- Add deterministic naming and overload handling rules.
- Add compile-check integration tests for generated C/C++ output.

4. Non-C backends
- Define per-language MVP contract (module layout, naming, ABI strategy).
- Implement one full backend end-to-end (recommended: Python or Rust) before broadening.
- Keep other backends as explicit "stub" status until feature-complete.

5. Test suite expansion
- Add parser golden tests (input -> expected AST).
- Add negative syntax tests with exact diagnostics.
- Add generated-code compile and runtime smoke tests for C backend.

## Conclusion
Wrapgen now has a usable parsing core and basic generation orchestration with mode-level tests. The C generator demonstrates the intended direction, but semantic validation and deterministic codegen behavior need substantial work. Non-C targets are currently scaffolding only. The project is at a solid foundation stage, with clear next steps to reach production-grade wrapper generation.
