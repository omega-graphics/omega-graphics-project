# OmegaSL Sema Error Location Audit

Audited 2026-03-21. Checks whether all `diagnostics->addError()` calls in `Sema.cpp` set `e->loc` so that `DiagnosticEngine::report` can produce CodeView output (source line + `^` underline).

## How ErrorLoc works

`ErrorLoc` (defined in `Error.h`) has `lineStart`, `lineEnd`, `colStart`, `colEnd`, all defaulting to 0. In `DiagnosticEngine::report` (`Error.cpp`), the location header and CodeView are gated by `e.loc.lineStart > 0`. A default-constructed `ErrorLoc{}` (all zeros) causes **both the `(line:col)` prefix and the source context to be silently skipped**, producing output like:

```
error: some message
```

instead of:

```
error (42:5): some message
  42 | float x = badCall();
     |           ^^^^^^^
```

Most error paths use `e->loc = node->loc.value_or(ErrorLoc{})`. If the AST node's `std::optional<ErrorLoc>` is populated (which it is for all nodes created by the current parser), this produces correct output. If the optional is empty, it falls back to zeros — no crash, but no location.

## Coverage: 41 / 44 (93%)

### Missing locations

| Sema.cpp line | Error type | Message | Available context |
|---|---|---|---|
| 916 | `TypeError` | `"Failed to perform sem on block statement"` | Statement `s` is in scope — could use `s->loc` |
| 936 | `TypeError` | `"Inconsistent return types in function block"` | Function decl `funcContext` and return type list `allTypes` are in scope — could use `funcContext->loc` or the loc of the first conflicting return |
| 1261 | `UnexpectedToken` | `"Cannot declare ast::Stmt of type: 0x..."` | Defensive default case in `performSemForGlobalDecl` switch — `decl` pointer is in scope but this path should be unreachable in practice |

### All error paths with locations set (41)

These all correctly use `e->loc = _expr->loc.value_or(ErrorLoc{})` or equivalent on the nearest AST node:

- Variable declaration type resolution failures
- Undeclared identifier in expression
- Member access on unknown struct field
- Swizzle component not found on vector type
- Binary expression type mismatch
- Buffer index type validation
- Vector index type inference
- All builtin function argument count and type checks (`make_float2/3/4`, `make_int2/3/4`, `make_uint2/3/4`, `sample`, `read`, `write`, `dot`, `cross`)
- User-defined function argument count mismatch
- Cast target type resolution
- Struct declaration: duplicate name, field type resolution, field uniqueness, invalid attributes
- Resource declaration: duplicate name, invalid type, invalid static qualifier
- Function declaration: parameter uniqueness, return type resolution, parameter type resolution
- Shader declaration: duplicate name, parameter uniqueness, resource map validation, attribute validation, return type checks
- VarDecl initializer type mismatch
