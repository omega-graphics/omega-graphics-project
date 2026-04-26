# Omega WrapGen Parser and Semantic Hardening Plan

Date: 2026-04-26

## Goal

Harden the Omega WrapGen front end so it can become a reliable source for the
object-oriented IR work. This plan focuses only on lexing, parsing, AST
ownership, diagnostics, symbol resolution, and semantic validation.

The desired end state is:

- parser instances are reentrant and own their state
- tokens and diagnostics carry source locations
- parser output is deterministic
- semantic checks reject invalid programs before generation
- tests pin both valid AST shape and invalid diagnostics

## Current State

`CURRENT_IMPLEMENTATION_EVALUATION.md` identifies the right broad risks:

- parser state is not instance-owned
- diagnostics are generic and have no line or column information
- AST memory ownership is raw-pointer based
- parameter order is unstable through `MapVec`
- duplicate declarations, duplicate methods, duplicate parameters, and overload
  rules are not fully checked
- lexer behavior is minimal and ignores some unexpected characters

The current code is slightly ahead of the evaluation in a few places:

- `interface` and `struct` parsing exist
- semantic checks now reject unknown types
- semantic checks reject namespace names used as types
- bare `void` is restricted to return types
- `void[]` is rejected
- duplicate class and struct fields are rejected
- class field/method name collisions are rejected

Those improvements should be preserved while the front end is hardened.

## Design Principles

1. Keep behavior stable while moving ownership and state boundaries.
   Refactors should first preserve existing AST dumps and generated outputs.

2. Improve diagnostics before expanding grammar.
   Better errors make later parser and semantic changes much easier to test.

3. Make ordering deterministic early.
   Stable parameter and declaration order is needed by generation, golden tests,
   and future OO IR construction.

4. Separate parsing from semantic validation.
   The parser should build syntax. Semantic checks should handle symbol, type,
   duplicate, and API-shape rules.

5. Add one rule at a time.
   Each new rejection should have at least one focused invalid fixture.

## Thin-Slice Implementation Plan

### Slice 0: Preserve Current Baseline

Goal: keep the current parser and semantic behavior pinned while making changes.

Tasks:

- keep the Phase 0 parser golden test and C compile smoke tests in place
- add negative syntax fixtures for the parser cases that already fail today
- document known accepted quirks, such as optional commas between parameters
- run `wrapgen-tests` after each later slice

Done when:

- current valid fixtures still pass
- current invalid fixtures still fail with the same diagnostic intent

### Slice 1: Source Locations in Tokens and Diagnostics

Goal: make every lexer, parser, and semantic diagnostic actionable.

Tasks:

- add a `SourceLocation` or `SourceRange` type with file, line, column, and byte
  offset fields
- extend `Tok` with start and end locations
- track line and column in `Lexer::nextTok`
- extend `Diagnostic` with optional source range and diagnostic code
- update lexer and parser errors to include locations
- update test helpers to compare important diagnostic text and locations

Done when:

- invalid fixtures print line and column information
- missing brace, missing colon, invalid token, unknown type, and invalid `void`
  tests assert useful locations

### Slice 2: Instance-Owned Parser State

Goal: make `Parser` reentrant and safe to instantiate more than once.

Tasks:

- move `TreeBuilder` and `TreeSemantics` ownership into `Parser`
- remove file-scope static `builder` and `semantics`
- make `TreeSemanticsContext` parser-owned for the duration of `beginParse`
- add a regression test that runs two parser instances sequentially in one
  process
- add a regression test that parses two different input streams without shared
  state bleeding across parses

Done when:

- no parser or semantic pass state is stored in file-scope mutable globals
- existing parser dumps remain unchanged for valid fixtures

### Slice 3: Deterministic Parameters

Goal: preserve callable parameter order and reject duplicates.

Tasks:

- replace `FuncDeclNode::params` with an ordered parameter vector
- introduce a small `ParamDecl` struct with name, type, and source location
- update parser, AST dumper, semantic checks, and generators
- reject duplicate parameter names in the semantic pass or parser
- add golden tests for multi-parameter functions and methods

Done when:

- generated C signatures preserve source parameter order
- duplicate parameter names produce a clear diagnostic
- AST dumps no longer depend on unordered-map iteration

### Slice 4: Lexer Hardening

Goal: make tokenization predictable and intentionally scoped.

Tasks:

- report an error for every unexpected character instead of silently ignoring it
- decide whether identifiers should support underscores now; if yes, implement
  `[A-Za-z_][A-Za-z0-9_]*`
- make `char` a lexer keyword if it remains a builtin type
- implement unterminated string diagnostics
- implement string escape handling or explicitly reject escapes with a clear
  error
- either implement block comments or remove/defer `TOK_BLOCKCOMMENT`
- add EOF-safe handling for comments and strings

Done when:

- invalid punctuation tests fail with exact source locations
- string and comment edge cases do not hang or read past EOF
- the lexer keyword list matches builtin type behavior

### Slice 5: Parser Error Boundaries and Recovery

Goal: make parser failures local and understandable.

Tasks:

- replace parser macros with helper methods that include expected token, actual
  token, and source range
- add targeted diagnostics for:
  - unexpected EOF in class, struct, interface, or namespace body
  - unexpected declaration keyword in a restricted body
  - missing return type after `func`
  - missing parameter type
  - missing field type
- add simple synchronization after top-level declaration errors if practical
- keep recovery conservative; it is acceptable to stop after one precise error
  initially

Done when:

- common malformed inputs produce specific errors instead of generic
  `Expected Keyword`
- diagnostics identify the construct being parsed

### Slice 6: AST Ownership and Lifetime

Goal: remove unmanaged AST leaks without forcing a full AST redesign.

Tasks:

- introduce an AST arena owned by `Parser` or a `TranslationUnit`
- allocate `DeclNode`, `Type`, and `TreeScope` through that arena
- keep raw observer pointers inside AST nodes if that minimizes churn
- make global builtin types immutable or arena-independent
- define the lifetime contract between parser, tree consumers, and generators

Done when:

- parser-created AST memory is released with the parser or translation unit
- no generator depends on AST nodes outliving the parse/generation phase

### Slice 7: Symbol Table Hardening

Goal: catch duplicate and ambiguous declarations before generation.

Tasks:

- replace the linear symbol vector lookup with a scoped symbol table
- assign each declaration a fully qualified name during semantic collection
- reject duplicate type declarations in the same scope
- reject duplicate namespaces or define clear namespace merge behavior
- reject function declarations that collide in unsupported ways
- reject class method duplicates until overload rules are designed
- reject interface method duplicates

Done when:

- duplicate class, struct, interface, namespace, free-function, and method
  fixtures fail with precise diagnostics
- valid shadowing between inner and outer scopes follows documented rules

### Slice 8: Type Resolution Model

Goal: make resolved types explicit enough for OO IR construction.

Tasks:

- introduce a resolved type representation separate from syntax-only `Type`
- store builtin, user type, pointer, reference, array, and const information
  without losing nested qualifiers
- resolve names to declaration symbols, not only a yes/no lookup result
- preserve namespace-as-type diagnostics
- add tests for nested namespace lookup, global fallback, and invalid out-of-scope
  lookup

Done when:

- semantic output can answer which symbol each user type refers to
- future OO IR code does not need to redo lexical name lookup

### Slice 9: Semantic Rule Completion for Current Grammar

Goal: fully validate the grammar WrapGen currently accepts.

Tasks:

- validate all class, struct, interface, namespace, and function name collisions
- validate duplicate field and method names consistently across all container
  types
- validate function parameter names and return types
- reject arrays or references where the C backend cannot currently generate
  valid output, or mark them with explicit unsupported diagnostics
- decide and document whether `void*` is allowed in fields and params before the
  OO IR pointer-policy work
- add diagnostics for currently generated invalid C signatures where possible

Done when:

- accepted `.owrap` files should not produce obviously invalid C signatures for
  the current supported backend subset
- unsupported-but-parsed constructs fail before generation with a clear message

### Slice 10: Structured Diagnostic Codes

Goal: make diagnostics stable enough for tests and editor integrations.

Tasks:

- define diagnostic code ranges, such as `WG-Lex-001`, `WG-Parse-001`, and
  `WG-Sema-001`
- add codes to all lexer, parser, and semantic diagnostics
- update tests to assert codes instead of exact full prose when helpful
- document diagnostic codes in a small reference table

Done when:

- diagnostic tests are resilient to wording improvements
- users can search for a diagnostic code in docs or tests

### Slice 11: Grammar Strictness Pass

Goal: intentionally choose which current quirks remain part of the language.

Tasks:

- decide whether commas between parameters and fields should stay optional
- decide whether trailing commas should be supported everywhere
- decide whether qualified names are still deferred
- add syntax tests for every accepted and rejected form
- update `LANGUAGE_REFERENCE.md` to match the implementation

Done when:

- the grammar reference and parser behavior match
- accidental parser quirks are either documented or removed

### Slice 12: Front-End API for OO IR

Goal: expose a clean semantic front-end result for the upcoming OO IR builder.

Tasks:

- create a `TranslationUnit` result object containing declarations, scopes,
  resolved symbols, resolved types, and diagnostics
- make parser consumers read from the translation unit where possible
- keep `TreeConsumer` compatibility temporarily if needed
- define which front-end invariants OO IR can rely on

Done when:

- OO IR construction can start from one checked front-end object
- generators no longer need to compensate for unresolved names or invalid types

## Testing Plan

Add tests in layers:

- lexer unit tests for token sequences, locations, invalid characters, strings,
  comments, and EOF cases
- parser golden tests for valid AST shape
- parser negative tests for malformed declarations and malformed types
- semantic negative tests for duplicates, unknown types, invalid `void`, invalid
  scopes, duplicate params, duplicate methods, and backend-unsupported type forms
- integration tests that ensure accepted files still generate C output

The most important fixtures should be small and single-purpose. Broad fixtures
are still useful for baseline coverage, but one-rule fixtures make diagnostics
much easier to preserve.

## Recommended Order

1. Source locations and instance-owned parser state
2. deterministic parameter storage
3. lexer hardening
4. parser diagnostics and ownership cleanup
5. scoped symbol table and duplicate detection
6. resolved type model
7. current-grammar semantic completion
8. structured diagnostic codes and grammar reference sync
9. front-end result object for OO IR

This order minimizes churn: first make the front end observable and stable, then
make ordering deterministic, then enforce increasingly rich semantic rules.

## Open Questions

- Should underscore identifiers be allowed before OO IR work starts?
- Should duplicate namespaces merge or fail?
- Should function overloads be rejected now and designed later?
- Should unsupported backend constructs be semantic errors, warnings, or
  backend-specific diagnostics?
- Should `TreeConsumer` remain as a streaming API, or should generation consume a
  complete checked translation unit?
