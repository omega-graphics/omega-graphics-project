# Omega WrapGen Language Reference

Date: 2026-04-05

This document describes the `.owrap` language accepted by the current implementation in `common/wrapgen`.

WrapGen is a declaration language. You describe headers, types, functions, methods, and namespaces for wrapper generation. There are no function bodies, statements, expressions, inheritance clauses, templates, or annotations in the current language.

## Quick Example

```owrap
header "./myHeader.h"

namespace Math {
  struct Bounds {
    width:double
    height:double,
    area:long
  }

  interface Drawable {
    func draw(x:int, y:int) void
    func area() double
  }

  class Vec {
    length:int
    func normalize() void
  }

  func add(v:int) int
}
```

## Lexical Rules

### Comments

Only line comments are supported:

```owrap
// This is a comment
```

Block comments are not implemented.

### Whitespace

Whitespace is mostly insignificant. Declarations are separated by keywords and braces, not by semicolons.

### Identifiers

Identifiers are currently limited to alphanumeric characters:

- Valid: `Foo`, `Vec3`, `A1`
- Invalid: `foo_bar`, `my-type`, `Math::Vec`

In practice this means:

- No underscores in names
- No explicit namespace qualification syntax
- No dotted names

### String Literals

String literals use double quotes:

```owrap
header "./myHeader.h"
```

Escape sequences are not implemented. The lexer reads characters until the next `"` character.

### Punctuation Tokens

The current lexer recognizes:

- `{` and `}`
- `(` and `)`
- `:` and `,`
- `*` and `&`
- `[` and `]`

Semicolons are not part of the language. At the moment, many other unexpected punctuation characters are silently ignored by the lexer instead of producing a hard error, so author `.owrap` files conservatively.

## Keywords

Reserved keywords in the current lexer:

- `class`
- `func`
- `const`
- `interface`
- `namespace`
- `header`
- `void`
- `int`
- `float`
- `long`
- `double`
- `struct`
- `string`

Implementation note: `char` is also treated as a builtin type later in semantic checking and C generation, even though it is not lexically reserved as a keyword.

## Grammar

The language accepted by the parser is approximately:

```ebnf
file            = { decl } ;

decl            = header_decl
                | namespace_decl
                | class_decl
                | struct_decl
                | interface_decl
                | func_decl ;

header_decl     = "header" string_literal ;

namespace_decl  = "namespace" ident "{" { decl } "}" ;

class_decl      = "class" ident "{" { class_member } "}" ;
class_member    = field_decl | func_decl ;

struct_decl     = "struct" ident "{" { field_decl } "}" ;

interface_decl  = "interface" ident "{" { func_decl } "}" ;

field_decl      = ident ":" type [ "," ] ;

func_decl       = "func" ident "(" [ param_list ] ")" type ;
param_list      = param { [ "," ] param } [ "," ] ;
param           = ident ":" type ;

type            = [ "const" ] type_name [ "*" | "&" ] { "[" "]" } ;
type_name       = ident | builtin_type ;
```

Two parser quirks matter in practice:

- Commas between parameters are optional.
- Commas between struct/class fields are optional.

These are all accepted:

```owrap
func f(a:int, b:int) void
func f(a:int b:int) void
func f(a:int, b:int,) void

struct Pair {
  a:int,
  b:int
}

struct Pair {
  a:int
  b:int
}
```

## Declarations

### `header`

Syntax:

```owrap
header "./some/header.h"
```

Meaning:

- Declares a source header dependency for the wrapped API.
- In the current C backend, `header` declarations become `#include` lines in the generated `.cpp` file.
- The language allows multiple `header` declarations.

### `namespace`

Syntax:

```owrap
namespace Math {
  func add(v:int) int
  struct Bounds {
    width:double
  }
}
```

Meaning:

- Creates a lexical scope for nested declarations.
- Namespace scope participates in type resolution.
- A namespace body can contain any declaration the parser accepts at file scope.

### `func`

Syntax:

```owrap
func name(param1:type1, param2:type2) returnType
```

Examples:

```owrap
func draw(x:int, y:int) void
func makeLong(v:long) long
func getScores() int[]
```

Meaning:

- At file scope or inside a namespace: declares a free function.
- Inside a class: declares an instance method.
- Inside an interface: declares an interface method.
- Return type is mandatory, including `void`.

Current limitations:

- No function bodies
- No constructors
- No static method syntax
- No overload resolution rules

### `class`

Syntax:

```owrap
class Config {
  name:string
  version:int
  func reload() void
}
```

Meaning:

- Declares an object-like wrapped type.
- A class body may contain:
  - fields written as `name:type`
  - methods written as `func ...`

Current semantic checks:

- Class field names must be unique.
- A class field name cannot match a method name in the same class.

Current limitations:

- No base classes or inheritance
- No constructors
- No static methods in source syntax
- No method-overload checks

### `struct`

Syntax:

```owrap
struct Pixel {
  r:int,
  g:int,
  b:int
  alpha:float
}
```

Meaning:

- Declares a plain data type with fields.
- A struct body may only contain fields.

Current semantic checks:

- Struct field names must be unique.

### `interface`

Syntax:

```owrap
interface Drawable {
  func draw(x:int, y:int) void
  func area() double
}
```

Meaning:

- Declares a method-only contract.
- An interface body may only contain `func` declarations.

In the current C backend, interfaces are lowered to a `self` pointer plus a vtable struct.

## Types

### Builtin Types

Currently recognized builtin types:

- `void`
- `int`
- `float`
- `long`
- `double`
- `string`
- `char`

`char` is implementation-supported but not currently listed as a lexer keyword.

### User Types

User-defined types are unqualified identifiers that resolve to visible:

- `class` declarations
- `struct` declarations
- `interface` declarations

### Type Forms

Supported type forms:

```owrap
int
const int
string
MyType
MyType*
MyType&
string[]
int[][]
const char*
const int[]
```

Current rules:

- `const` must appear before the base type name.
- Only one `*` or `&` suffix is supported.
- Array suffixes come after the base type and pointer/reference marker.
- Multiple array suffixes are accepted.

Examples:

```owrap
func f(a:const int) void
func f(a:char*) void
func f(a:int*[]) void
func f() string[][]
```

Unsupported type syntax:

- `A::B`
- `vector<int>`
- `T**`
- `T&&`
- function pointer types
- aliases, enums, unions, or generics

## Scope and Name Resolution

Type lookup is lexical and unqualified.

The semantic pass resolves a type name by walking outward from the current scope to its parents until global scope is reached.

That means:

- A declaration can refer to types in its own scope.
- A declaration can refer to types from enclosing namespaces.
- A declaration inside a namespace can refer to global types.
- A declaration cannot explicitly name `OtherNamespace::Type`, because qualified names are not supported.
- A declaration outside a namespace cannot refer to a type that exists only inside that namespace unless an unqualified name in scope also matches.

Example:

```owrap
class GlobalType {
  value:int
}

namespace Math {
  struct Vec {
    x:int
    y:int
  }

  class Painter {
    target:Vec
    other:GlobalType
  }
}

class Outside {
  target:Vec
}
```

In the current implementation:

- `Painter.target:Vec` is valid.
- `Painter.other:GlobalType` is valid.
- `Outside.target:Vec` is invalid because `Vec` is only visible inside `Math`.

Using a namespace name where a type is expected is also a semantic error.

## Implemented Semantic Checks

The current semantic pass enforces:

- Unknown type names are rejected.
- Namespace names cannot be used as types.
- Bare `void` is only valid as a function return type.
- `void` cannot be used as a field type.
- `void` cannot be used as a parameter type.
- `void[]` is invalid.
- Duplicate class field names are rejected.
- Duplicate struct field names are rejected.
- Class field names cannot collide with method names.

Example invalid cases:

```owrap
struct InvalidVoidStruct {
  payload:void
}

class InvalidVoidMethods {
  func setArg(v:void) void
  func getArray() void[]
}

class UnknownTypeCase {
  value:Vec3
  func apply(v:Vec3) void
}
```

## Current Backend Interpretation

This section is not language syntax, but it helps explain what declarations mean today.

### C Backend

The C backend is the only backend with substantive wrapper generation at the moment.

Current behavior:

- `header` becomes an include in generated source.
- `class` becomes an opaque C handle plus instance-method wrappers.
- `class` fields become getter/setter wrappers.
- `const` class fields get a getter only.
- `struct` becomes a C struct declaration in the generated header.
- `interface` becomes a C struct with `void *self` and a vtable pointer.
- `string` maps to `const char *`.
- Arrays map to generated `OmegaArray_*` view structs.

### Python / Go / Java / Swift / Rust Backends

These backends currently emit skeleton output files only. They do not yet implement real type lowering or binding generation.

## Diagnostics

Parser and semantic errors are reported through the diagnostic buffer and written to stdout during parser finalization.

Current characteristics:

- Parse errors are generic, for example `Expected Identifier`.
- Semantic errors are more descriptive, for example `Unknown type 'Vec3' ...`.
- Diagnostics do not currently include line or column information.

## Known Limitations and Quirks

The following behaviors are important when authoring `.owrap` files today:

- Parameter names are stored in `OmegaCommon::MapVec`, which is currently an alias for `std::unordered_map`.
- Parameter order is therefore not guaranteed to be preserved during generation.
- Duplicate parameter names are not explicitly diagnosed.
- Duplicate methods are not explicitly diagnosed.
- Duplicate declarations across a namespace or file are not explicitly diagnosed.
- Qualified names such as `Math::Vec` are not supported.
- Non-alphanumeric identifiers are not supported.
- Block comments are not supported.
- String escapes are not supported.
- Many unexpected punctuation characters are ignored by the lexer instead of being rejected.

## Authoring Recommendations

Until the language and generators are hardened, the safest authoring style is:

- Use simple alphanumeric names only.
- Keep all referenced types in the same namespace or an enclosing scope.
- Avoid overloads and duplicate method names.
- Keep parameter names unique.
- Prefer explicit commas even though they are optional.
- Avoid relying on punctuation that is not part of the documented grammar.
- Use `void` only as a return type, or as part of a pointer/reference type such as `void*`.
