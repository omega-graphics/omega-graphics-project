# JSON API Completion Plan

> **Status: proposed, not started.** This plan completes the `OmegaCommon::JSON`
> value type. Three design forks were resolved with the developer up front:
> numbers become a **tagged int-or-double**, parse failures are reported through
> a new **`TryParse` → `Result<JSON, String>`** (the house pattern, same as
> `OmegaCommon::Img`'s `Result<BitmapImage, std::string>`), and the work ships as
> **one phased plan** that fixes the ownership foundation first and layers the new
> API on top. No code lands until this plan is reviewed.

## Current State

The JSON layer is implemented in:

- `common/include/omega-common/json.h` (145 lines)
- `common/src/json.cpp` (286 lines)

It is a single `JSON` value-node class backed by RapidJSON through a private
`RapidJSONBridge`. Storage is a tagged union (`enum { STRING, ARRAY, MAP, NUMBER,
BOOLEAN, UNKNOWN }`) over:

```
char *                       str       // STRING
bool                         b         // BOOLEAN
OmegaCommon::Vector<JSON> *  array     // ARRAY
OmegaCommon::Map<String,JSON>* map     // MAP
int                          number    // NUMBER   (typedef int JNumber)
```

### What Works

- Construct from `const char*`, `const String&`, `bool`,
  `std::initializer_list<JSON>` (array), `std::map<String,JSON>` (object).
- Type queries: `isString()`, `isArray()`, `isNumber()`, `isMap()` (all `const`).
- Reads: `asString() → StrRef`, `asVector() → ArrayRef<JSON>`,
  `asMap() → MapRef<String,JSON>`, `asFloat() → float`, `asBool() → bool&`.
- Mutation: `operator[](StrRef) → JSON&`, `insert(pair) → map_iterator`,
  `push_back(const JSON&)`.
- `static JSON parse(String)`, `static JSON parse(std::istream&)`,
  `static String serialize(JSON&)`, `static void serialize(JSON&, std::ostream&)`,
  plus `operator>>` / `operator<<`.
- A declared-but-unwired `JSONConvertible` interface (`toJSON`/`fromJSON`) and the
  `IJSONConvertible` macro.

### Gaps

**Tier 0 — correctness (these are bugs, not missing features):**

1. **Container nodes leak.** `Data::_destroy()` is defined at
   `common/src/json.cpp:238` but is **never called**; `~JSON() = default`
   (`json.h:121`) never frees the owned `str` / `map` / `array` allocations.
2. **No Rule-of-Five → shallow copies of owning pointers.** No copy ctor, move
   ctor, or `operator=` exists. `push_back(const JSON&)`, `fromRapid` returning by
   value, and the by-value `std::map` constructor all shallow-copy the union
   pointers, so two nodes alias the same heap object. Today that is an aliasing +
   leak hazard; the instant a real destructor lands it becomes a double-free.
3. **`parse` failure calls `std::exit(1)`** (`json.cpp:20-23`). A malformed config
   file hard-kills the host process. This is the only `std::exit` in all of
   `common/src`.

**Tier 1 — type-system completeness:**

4. **Numbers are `int`-only.** Parsing `3.14` truncates to `3`
   (`json.cpp:114`); `asFloat()` casts that int. Lossy for an engine that moves
   positions, colors, and matrices through config/serialized data.
5. **No number constructor at all**, and `JSON j(42)` silently selects
   `JSON(bool)` via the int→bool standard conversion, producing `true`.
6. **No explicit null, no `isBool()`, no `asInt()`.** `UNKNOWN` doubles as null
   with no `isNull()` query.
7. **Const-hostile reads.** Every accessor is non-`const`; a `const JSON&` cannot
   be read, and `serialize` `const_cast`s internally (`json.cpp:33`).

**Tier 2 — ergonomics & the dangling interface:**

8. **`operator[]` silently inserts** (`data.map->operator[]`), so a missing-key
   read mutates the tree. No `contains` / `find` / `at` / `size` / `empty`.
9. **Serialize is hardcoded `PrettyWriter`** (`json.cpp:153`) — no compact mode.
10. **`JSONConvertible` is unwired** — nothing in `JSON` accepts or produces one.

## Design References

- `OmegaCommon::Result<T,E>` (`common/include/omega-common/utils.h:790`) — the
  documented house result type, already adopted by `OmegaCommon::Img`.
- `common/.plans/ImgCodec-API-Extension-Proposal.md` — the sibling
  API-completion precedent in this module (`Result<BitmapImage, std::string>`,
  RAII storage wrapper, capability registry).
- RapidJSON (`common/deps/rapidjson`) — existing, no new dependency required.

## Goals

- A `JSON` that is a correct C++ value type: deep-copy + move, no leaks, no
  aliasing.
- Lossless numbers for both integers and reals.
- Parse failures returned as data, never a process exit.
- Safe, `const`-correct reads and non-mutating lookups.
- A live bridge between `JSON` and `JSONConvertible`.

## Non-Goals

- JSON5 / comments / trailing commas (RapidJSON default grammar only).
- Schema validation, JSON Pointer, or JSON Patch.
- A streaming/SAX public API (the RapidJSON SAX layer stays private).
- Changing the `JSON(std::initializer_list<JSON>)` array shorthand or the
  `IJSONConvertible` macro spelling (kept for source compatibility).

## Proposed Public API

C++17 ceiling throughout (no designated initializers — named locals per
`AGENTS.md`), `typedef` over `using`, PascalCase static methods,
`OMEGACOMMON_EXPORT` on exported entities.

### 1. Ownership foundation (Rule of Five)

```cpp
JSON(const JSON & other);             // deep copy of str / array / map
JSON(JSON && other) noexcept;         // steal pointer, leave other UNKNOWN
JSON & operator=(const JSON & other);
JSON & operator=(JSON && other) noexcept;
~JSON();                              // calls data._destroy(type)
```

`~JSON` finally calls the existing `Data::_destroy(type)`. Copy deep-clones the
active arm; move transfers the pointer and resets the source to `UNKNOWN` so its
destructor is a no-op.

### 2. Number model — tagged int OR double

The number arm becomes a small tagged value so integers and reals both round-trip
losslessly:

```cpp
// inside Data
struct Num { bool isReal; long long i; double d; } number;
```

```cpp
JSON(int v);
JSON(long long v);
JSON(double v);

bool  isInt()  const;   // NUMBER && !isReal
bool  isReal() const;   // NUMBER && isReal
long long asInt();      // truncates if currently real
double    asDouble();
float     asFloat();    // retained; narrows asDouble()
```

`fromRapid` records `IsInt64/IsUint64` as integer and `IsDouble` as real;
`writeNode` emits `Int64` vs `Double` per the tag.

### 3. Null and bool

```cpp
JSON(std::nullptr_t);   // explicit NULL node (distinct from default UNKNOWN)
bool isNull() const;
bool isBool() const;
```

A dedicated `NUL` enum member is added so "explicitly null" and "uninitialized"
are distinguishable; the serializer writes `Null()` for both.

### 4. Const-correct, non-mutating reads

```cpp
// const overloads of every accessor
StrRef               asString()  const;
ArrayRef<JSON>       asVector()  const;
MapRef<String,JSON>  asMap()     const;
const JSON &         operator[](StrRef key) const;  // asserts present, no insert

// object lookups that do not mutate
bool          contains(StrRef key) const;
JSON *        find(StrRef key);          // nullptr if absent
const JSON *  find(StrRef key) const;
const JSON &  at(StrRef key) const;      // asserts present

// container introspection
size_t size()  const;   // members for MAP, elements for ARRAY
bool   empty() const;
```

`operator[](StrRef)` (non-const) keeps its current insert-on-miss behavior for
builder ergonomics; the const overload and `find`/`at` cover safe reads.

### 5. Error-returning parse — `TryParse`

```cpp
static Result<JSON, String> TryParse(StrRef source);
static Result<JSON, String> TryParse(std::istream & in);

// retained, now thin wrappers; assert(false) in debug on error, UNKNOWN in release
static JSON parse(String str);
static JSON parse(std::istream & in);
```

`RapidJSONBridge::fail` stops calling `std::exit`; it returns the error string up
to `TryParse`. The legacy `parse` wraps `TryParse` so existing call sites compile
unchanged while new code can branch on `isErr()`.

### 6. Serialize options

```cpp
static String serialize(JSON & json, bool pretty = true);
static void   serialize(JSON & json, std::ostream & out, bool pretty = true);
```

`pretty == false` swaps `PrettyWriter` for RapidJSON's compact `Writer`. Default
`true` preserves current output byte-for-byte.

### 7. JSONConvertible bridge

```cpp
template<class T>
static JSON From(T & v) { JSON j; v.toJSON(j); return j; }   // T : JSONConvertible

template<class T>
void into(T & v) { v.fromJSON(*this); }
```

Connects the existing `IJSONConvertible` macro so user types round-trip without a
manual `toJSON`/`fromJSON` dance at each call site.

## Implementation Plan

### Phase 1 — Ownership foundation (Tier 0, do first)

Add destructor + copy/move ctor + copy/move assignment; wire `~JSON` to
`Data::_destroy`. Convert `Num` storage scaffold so later phases don't re-touch
the union. **Verification:** an ASan round-trip test that parses, copies,
reassigns, and drops nested objects/arrays with zero leaks and zero double-frees.
This phase fixes bugs #1 and #2 and is the prerequisite for everything else.

### Phase 2 — Number model

Land the tagged `Num`, the `int`/`long long`/`double` constructors, `isInt`/
`isReal`/`asInt`/`asDouble`, and the `fromRapid`/`writeNode` tag handling. Fixes
#4 and #5. **Verification:** round-trip `3`, `2147483648`, `3.14`, `-0.0`,
`1e308` through parse → serialize and assert exact text.

### Phase 3 — Null, bool, const reads, lookups

Add `NUL`, `JSON(nullptr_t)`, `isNull`/`isBool`, the `const` accessor overloads,
and `contains`/`find`/`at`/`size`/`empty`. Fixes #6, #7, #8.

### Phase 4 — Error-returning parse

Introduce `TryParse → Result<JSON,String>`; remove `std::exit`; repoint `parse`
as a wrapper. Fixes #3. **Verification:** malformed input yields `isErr()` with a
RapidJSON message + offset and the process stays alive.

### Phase 5 — Serialize options + convertible bridge

Add the `pretty` flag and the `From`/`into` templates. Fixes #9, #10.

## Testing Plan

- New `common` test exercising: deep-copy independence, move-then-use-source,
  number round-trips, null vs unknown, missing-key `find` returning `nullptr`,
  `TryParse` error path, compact vs pretty bytes, and a `JSONConvertible`
  round-trip. Match the existing `common` test layout/build wiring.
- Run under ASan for Phase 1 leak/double-free coverage.

## Recommended First Patch

Phase 1 only: destructor + Rule-of-Five + the leak fix, with the ASan round-trip
test. It is the smallest reviewable slice, it removes the most dangerous bug
(silent aliasing that becomes a double-free the moment ownership is corrected),
and it unblocks every later phase. If Phase 1's ownership semantics are right, the
remaining phases are additive.
