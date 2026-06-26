# JSON API Completion Plan

> **Status: COMPLETE — all 5 phases landed + verified (2026-06-20).**
> The `OmegaCommon::JSON` value type is finished: correct ownership, lossless
> numbers, null/bool, const-correct + non-mutating reads, error-returning parse,
> serialize options, and a live JSONConvertible bridge. The downstream LSP
> migration (replace `gte/omegasl/lsp/Json.cpp` with this API) is now unblocked
> but tracked separately.
> This plan completes the `OmegaCommon::JSON` value type. Three design forks were
> resolved with the developer up front: numbers become a **tagged int-or-double**,
> parse failures are reported through a new **`TryParse` → `Result<JSON, String>`**
> (the house pattern, same as `OmegaCommon::Img`'s `Result<BitmapImage,
> std::string>`), and the work ships as **one phased plan** that fixes the
> ownership foundation first and layers the new API on top.

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

### Phase 1 — Ownership foundation (Tier 0) — **Complete (2026-06-20)**

Added the destructor + copy ctor + move ctor + copy/move assignment to
`JSON` (`json.h` special-members block, `json.cpp` definitions) and wired
`~JSON` to the previously-dead `Data::_destroy`. Copy deep-clones the active
union arm (String via `Data(StrRef)`, Array/Map via the container copy ctors
which recurse through `JSON`'s own copy ctor); move bitwise-steals the union and
resets the source to `UNKNOWN` so its destructor is a no-op. Both assignments are
self-assignment guarded; copy-assign clones before releasing so a throw leaves
`*this` intact. Fixes bugs #1 (leak) and #2 (shallow-copy aliasing).

The `Num` storage change was deliberately **deferred to Phase 2** — leaving the
`int number` arm untouched kept Phase 1 to pure ownership and avoided writing
copy/move logic for a number layout that Phase 2 replaces anyway. (Number arms
copy trivially regardless, so Phase 2 does not re-touch the special members.)

**Verification (all passing):**
- `common/tests/JSONLifecycleTest.cpp` — 22/22 checks: deep-copy independence
  (top-level + nested + array growth), move transfer + empty source, copy/move
  assignment, self-assignment safety, copy-serializes-identically.
- AddressSanitizer build: no double-free / use-after-free.
- macOS `leaks --atExit`: `0 leaks for 0 total leaked bytes` (LSan is unreliable
  on macOS, so the native `leaks` tool is the leak oracle here).
- Regression: `omega-assetc` rebuilds clean and the 7 `assetc` integration tests
  (which `JSON::parse` real configs) pass.
- Test wired into both build descriptions: `common/tests/CMakeLists.txt`
  (`add_subdirectory(tests)` in `common/CMakeLists.txt`, target
  `common-core-tests`) and `common/AUTOM.build` (`json-lifecycle-test`).

### Phase 2 — Number model — **Complete (2026-06-20)**

Replaced the `int number` union arm with a tagged `struct Num { bool isReal;
long long i; double d; }` (trivially copyable, so Phase 1's special members were
untouched as predicted). Added `JSON(int)` / `JSON(long long)` / `JSON(double)`
constructors, `isInt`/`isReal`, and `asInt`/`asDouble` (with `asFloat` retained,
now narrowing `asDouble`). `fromRapid` routes Int/Uint/Int64 to the integer arm
and doubles to the real arm, with unsigned values above the signed-64 range kept
as reals rather than wrapping; `writeNode` emits `Int64` vs `Double` per the tag.
Fixes #4 (lossy numbers) and #5 (no numeric ctor / `JSON(42)` becoming a bool).

**Verification (all passing):**
- `common/tests/JSONNumberTest.cpp` (target `json-number-test`, wired in CMake +
  `AUTOM.build`): exact-text round-trip of `3`, `2147483648`, `3.14`, `-0.0`,
  `1e308`; integer/real tag + value checks; `-0.0` sign preserved (`signbit`);
  the int/long long/double constructors; `asFloat` narrowing; and the footgun
  fix (`JSON(42)` is a number, `JSON(true)` is still a bool).
- AddressSanitizer: clean. macOS `leaks`: `0 leaks`.
- Regression: `assetc` integration (7 tests) still green through the rewritten
  `fromRapid`/`writeNode`.

**Follow-up surfaced (out of scope, flagged separately):**
`gte/omegasl/lsp/LspServer.cpp` reads JSON-RPC request ids via `asFloat()`
(`idToJson` ~L58, `getInt` ~L46) — float's 24-bit mantissa corrupts integer ids
above 2^24. Now trivially fixable with the new `asInt()`. Tracked as a spawned
task; not part of this phase (different module).

### Phase 3 — Null, bool, const reads, lookups — **Complete (2026-06-20)**

Added the `NUL` enum tag + `JSON(std::nullptr_t)` (explicit null, distinct from
the default UNKNOWN node), `isNull`/`isBool`, and routed `fromRapid`'s `IsNull`
to `JSON(nullptr)` so parsed `null` reports `isNull()` (writeNode already
fell through to `Null()`, so NUL/UNKNOWN both serialize as `null`). Made the
read accessors `const`-qualified (`asString`/`asVector`/`asMap`/`asInt`/
`asDouble`/`asFloat`) — safe because the union holds raw pointers, so object
constness does not propagate to the pointed-to container — and added a const
`asBool()` returning by value alongside the existing mutable `bool& asBool()`.
Added non-mutating lookups: `const operator[]` (asserts present, delegates to
`at`), `contains`, `find`/`find const` (nullptr when absent), `at`/`at const`
(assert present), `size`, `empty`. Fixes #6, #7, #8.

**Verification (all passing):**
- `common/tests/JSONLookupTest.cpp` (target `json-lookup-test`, wired in CMake +
  `AUTOM.build`): explicit null vs default node + parsed/serialized null; `isBool`
  and const `asBool`; full `const JSON&` read path (operator[]/at/asMap/asInt/
  asDouble/asBool/size); `find`/`contains` locate-but-never-insert (size
  unchanged) contrasted with mutating `operator[]` that does insert; mutate via
  `find*`/`at` without growing the map; `size`/`empty` for maps and arrays.
- AddressSanitizer: clean. macOS `leaks`: `0 leaks`.
- Regression: `omega-assetc` rebuilt (its `main.cpp` calls the now-`const`
  `asMap`/`asVector`/`asString`) and the 7 `assetc` integration tests pass.

Note: the const accessors return the existing view types (`StrRef`/`ArrayRef`/
`MapRef`); those are read-only interfaces, so handing one out from a const node
is fine even though `MapRef`/`ArrayRef` wrap a non-const ref internally.

### Phase 4 — Error-returning parse — **Complete (2026-06-20)**

Added `static Result<JSON,String> TryParse(StrRef)` / `TryParse(std::istream&)`.
The `RapidJSONBridge` parse overloads now return `Result<JSON,String>` (the
`std::exit(1)`-calling `fail()` helper is deleted; a shared `errorMessage()`
builds the RapidJSON description + byte offset). `JSON::parse` is now a thin
wrapper over `TryParse` that asserts on error in debug and returns an empty node
in release — its public signature is unchanged, so existing call sites (assetc,
`operator>>`, the LSP) compile untouched. `fromRapid`'s unreachable
"unsupported value type" path (parse errors are caught before it runs) became an
`assert(false)` + `return JSON(nullptr)` instead of an exit. Fixes #3.

**Verification (all passing):**
- `common/tests/JSONParseResultTest.cpp` (target `json-parse-result-test`, wired
  in CMake + `AUTOM.build`): ok path (String + istream) returns the parsed value;
  error path yields `isErr()` with a non-empty message containing the byte offset
  (observed e.g. `Missing a name for object member. at offset 2`); multiple
  malformed inputs in sequence prove the process no longer exits; `Result::valueOr`
  fallback; legacy `parse()` still parses valid input.
- AddressSanitizer: clean. macOS `leaks`: `0 leaks` — including the error paths,
  where the RapidJSON `Document` and the error `String` are both freed.
- Regression: `omega-assetc` rebuilt + 7 `assetc` integration tests pass;
  `std::exit` is gone from `common/src/json.cpp` entirely.

### Phase 5 — Serialize options + convertible bridge — **Complete (2026-06-20)**

Added the `pretty` flag to both `serialize` overloads (default `true`, preserving
current pretty output byte-for-byte; `false` swaps `PrettyWriter` for RapidJSON's
compact `Writer` — `writeNode` is already a template over the writer type). Wired
the dangling `JSONConvertible` via the templates `static JSON From(T&)`
(calls `v.toJSON`) and `void into(T&)` (calls `v.fromJSON`) — duck-typed on T, so
any type with the two methods works, not only `JSONConvertible` subclasses. Fixes
#9, #10.

**Addition (surfaced by the LSP-migration analysis):** added empty-container
factories `static JSON Object()` / `static JSON Array()`. Without them, a
`toJSON` implementation had no clean way to start building a container (the only
paths were `JSON(std::map{})` / `JSON(std::initializer_list{})`). They make the
convertible/builder story usable and cover the LSP's `Json::object()`/`array()`
shape. Not in the original Proposed Public API; folded into this phase.

**Verification (all passing):**
- `common/tests/JSONConvertTest.cpp` (target `json-convert-test`, wired in CMake +
  `AUTOM.build`): compact (`{"a":"b"}`, no whitespace) vs pretty (has newlines)
  and `serialize(json) == serialize(json, true)` backward compat; `Object()`/
  `Array()` empty + buildable + compact-serialized; a real `IJSONConvertible`
  `Point` round-tripping through `From`/`into` and through a serialize→parse→into
  cycle.
- AddressSanitizer: clean. macOS `leaks`: `0 leaks` (the new factories + `From`
  allocate containers that are freed).
- Regression: 7 `assetc` integration tests pass — default-pretty serialize output
  is unchanged.

---

## Outcome

All five phases complete. `OmegaCommon::JSON` went from a leaking, shallow-copying,
int-only, process-exiting value type to a correct one: deep-copy/move ownership
(no leaks, no aliasing), lossless tagged int-or-double numbers, explicit null +
`isBool`, const-correct and non-mutating reads (`contains`/`find`/`at`/`size`/
`empty`), `TryParse → Result<JSON,String>` (no more `std::exit`), compact-or-pretty
serialization, empty-container factories, and a working `JSONConvertible` bridge.
Five focused test binaries (`json-{lifecycle,number,lookup,parse-result,convert}-test`,
grouped under the `common-core-tests` target) cover it, each verified under ASan
and the macOS `leaks` tool, with the `assetc` integration suite green throughout.

**Open follow-up (separate effort):** migrate `gte/omegasl/lsp/` off its bespoke
`Json` (`Json.cpp`/`Json.h`) onto this API now that parity is reached — including
the spawned `asInt()` request-id precision fix, which that migration subsumes.

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
