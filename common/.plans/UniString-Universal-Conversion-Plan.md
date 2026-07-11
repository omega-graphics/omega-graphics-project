# UniString as the Universal String-Conversion Type

**Status: PLANNED — not yet started. Do not implement until greenlit.** This
document captures the intended shape so the work can begin cleanly later; the
design decisions in §6 (Open Questions) should be resolved with the developer
before any code lands.

## 1. Motivation

String/encoding conversion is currently done ad-hoc at every native boundary,
with several independent, sometimes-incorrect implementations of the same
UTF-8 ↔ UTF-16 conversion. `OmegaCommon::UniString` (ICU-backed, UTF-16 internal)
already does correct conversions; the goal is to **finish** it so it can be the
single conversion hub everywhere, and retire the scattered helpers.

### The duplication today (grounded inventory)

| Location | Helper / pattern | Encoding basis | Notes |
|----------|------------------|----------------|-------|
| `wtk/src/Native/win/WinUtils.{h,cpp}` | `cpp_str_to_cpp_wstr` | **ATL `CStringW` — ACP, not UTF-8** | latent bug: non-ASCII silently corrupted |
| `wtk/src/Native/win/WinDialog.cpp` | local `widen` / `narrow` | `MultiByteToWideChar(CP_UTF8)` | correct but private, duplicated |
| `wtk/src/Native/win/WinNote.cpp` | local `widen` / `narrow` | `MultiByteToWideChar(CP_UTF8)` | duplicate of the above |
| `wtk/src/Native/win/WinClipboard.cpp` | `utf8ToWide` + inline `WideCharToMultiByte` | `CP_UTF8` | third copy |
| `wtk/src/Native/win/WinMenu.cpp` | `composeMenuText`, `std::wstring` handling | mixed | |
| `wtk/src/Native/win/WinApp.cpp` | inline `MultiByteToWideChar` | `CP_UTF8` | |
| `wtk/src/Composition/backend/dx/DWriteFontEngine.cpp` | inline `MultiByteToWideChar` | `CP_UTF8` | DirectWrite family names |
| `wtk/src/Native/macos/CocoaUtils.mm` | `common_string_to_ns_string` / `ns_string_to_common_string` | `NSUTF8StringEncoding` | correct; used across all Cocoa*.mm |
| GTK layer | (mostly pass-through) | GLib is UTF-8 native | audit only; likely little to change |

Out of scope for the first pass (separate modules, migrate later): `gte/` D3D12
+ test window, `kreate/` Win32 window, `common/src/win/net-win.cpp`.

## 2. Current UniString surface (`common/include/omega-common/unicode.h`)

- `UniString()` / `UniString(const char * utf8)`
- `static fromUTF8(const char*)`, `static fromUTF32(const char32_t*, len)`
- `toUTF8() -> String`, `toUTF32() -> UString`  *(added alongside the dialog fixes)*
- `length()`, `getBuffer() -> const char16_t *`
- private `UniString(std::u16string)`
- Internally `std::u16string data_`, ICU (`u_strFromUTF8` / `u_strToUTF8`, etc.)

Backing store is UTF-16, which equals Windows `wchar_t`/`WCHAR` — so Win32 W-API
interop can be **zero-cost** (a reinterpret of the `char16_t` buffer), not another
ICU round-trip. That is the key efficiency point the design should preserve.

## 3. Gaps to make it usable "anywhere"

1. **No `std::wstring` / `wchar_t` interop.** Win32 W-APIs want `const wchar_t*`.
   On Windows `wchar_t` is 16-bit (== our buffer); on Linux/macOS it is 32-bit.
   Need a Windows-flavored accessor that is documented/guarded accordingly.
2. **No construction from `OmegaCommon::String` / `StrRef`** — only `const char*`.
3. **No public construction from UTF-16** (`const char16_t*` / `WString` /
   `std::wstring`) for the reverse (native → UniString) direction.
4. **No value ergonomics**: `empty()`, `size()`, `operator==`/`!=`, ordering,
   `std::hash` (needed to use it as a map key), concatenation/`substr` if it is
   meant to be more than a bridge (see §6.1).
5. **Error signaling** is "return empty on failure" — fine for a bridge, but a
   `Result<UniString>` variant may be wanted for fallible inputs.

## 4. Proposed surface (to be confirmed in §6)

Framing UniString as an **immutable conversion/bridge value type** with rich
accessors into every encoding (not a general mutable string — see §6.1):

```cpp
// Construction (all encodings in)
static UniString fromUTF8 (StrRef utf8);          // + keep const char* overload
static UniString fromUTF16(const char16_t*, int32_t len);
static UniString fromUTF16(const WString &);
static UniString fromUTF32(const UString &);       // + keep raw overload
#ifdef _WIN32
static UniString fromWide (const wchar_t*, int32_t len);  // 16-bit memcpy path
#endif

// Accessors (all encodings out)
String   toUTF8()  const;   // == toString()
WString  toUTF16() const;   // == the backing store, cheap
UString  toUTF32() const;
const char16_t * u16() const;   // == getBuffer(), for LPCWSTR-style calls
#ifdef _WIN32
const wchar_t *  w_str()      const;  // zero-cost reinterpret of u16()
std::wstring     toWString()  const;  // 16-bit copy on Windows
#endif

// Value semantics
bool empty() const; int32_t length() const;
bool operator==(const UniString&) const; // + != and a hash specialization
```

Platform bridges (NSString on macOS, HSTRING/BSTR if ever needed) **stay in the
platform layer** and consume UniString accessors — `common/` must not gain a
Cocoa or Win32 dependency (hard constraint, see §6.3).

## 5. Phased implementation (when greenlit)

- **Phase 1 — grow the type.** Add the §4 constructors/accessors + `std::hash`
  to `unicode.{h,cpp}`. Keep the cheap Windows UTF-16↔wchar_t path non-ICU. Add
  unit coverage (round-trips, empty, non-BMP, invalid input). No call-site
  changes yet. *(small feature; ~<300 LOC)*
- **Phase 2 — Win consolidation.** Replace `cpp_str_to_cpp_wstr` (fixes the ACP
  bug), the two `widen`/`narrow` copies, `utf8ToWide`, and the inline
  `MultiByteToWideChar`/`WideCharToMultiByte` sites in the `wtk` Win layer with
  UniString. Delete `WinUtils`'s ATL dependency.
- **Phase 3 — macOS consolidation.** Reimplement `common_string_to_ns_string` /
  `ns_string_to_common_string` on top of UniString accessors (kept in the Cocoa
  layer). Optionally add a small `NSString*(<->)UniString` inline in a Cocoa
  header.
- **Phase 4 — DirectWrite + GTK audit.** Route `DWriteFontEngine` family-name
  conversion through UniString; audit GTK for any non-UTF-8 assumptions.
- **Phase 5 — other modules (optional/later).** `gte/`, `kreate/`,
  `common/src/win/net-win.cpp`.

Migration tactic: in each phase, first make the old helper a thin shim that
delegates to UniString (proves parity with zero call-site churn), then inline
and delete the shim.

## 6. Open Questions (resolve before Phase 1)

1. **Scope: bridge vs full string.** Recommendation: keep UniString an
   *immutable conversion value type* (accessors + equality/hash), and leave
   mutable text manipulation to `String`/`WString`. Making it a full
   general-purpose Unicode string (indexing, concatenation, code-point iteration,
   normalization) is a much larger surface that ICU's `UnicodeString` already
   covers — do we want that, or is the bridge role the target?
2. **`wchar_t` on non-Windows.** `toWString()`/`w_str()` only make sense where
   `wchar_t` is 16-bit (Windows). Guard them `#ifdef _WIN32`, or provide a
   32-bit-correct `std::wstring` path elsewhere? (Win32 APIs are the only
   consumers, so guarding is likely fine.)
3. **`common/` dependency boundary.** Confirm platform bridges (NSString, etc.)
   stay out of `common/` and live in the consuming layer. (Strongly recommended;
   listed here to make it an explicit decision.)
4. **Error handling.** Keep "empty on failure", or add `Result<UniString>` for
   fallible decode? Affects the constructor signatures in §4.
5. **StrRef interplay.** Should read-only parameters take `UStrRef`/`WStrRef`
   (the non-owning views) to avoid copies, per the AGENTS.md ADT guidance?

## 7. File Change Summary (projected)

### Modified
- `common/include/omega-common/unicode.h` — §4 surface
- `common/src/unicode/UniString.cpp` — implementations (cheap Win path + ICU)
- `wtk/src/Native/win/WinUtils.{h,cpp}` — remove ATL helper (Phase 2)
- `wtk/src/Native/win/{WinDialog,WinNote,WinClipboard,WinMenu,WinApp}.cpp` — use UniString
- `wtk/src/Composition/backend/dx/DWriteFontEngine.cpp` — use UniString
- `wtk/src/Native/macos/CocoaUtils.mm` — reimplement bridges on UniString

### New
- `common/tests/...` — UniString conversion unit tests (encoding round-trips)

### Unchanged (by design)
- `common/` gains no Cocoa/Win32 dependency; platform bridges stay in `wtk`.
