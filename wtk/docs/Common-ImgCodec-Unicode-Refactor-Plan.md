# Refactor Plan — Move `ImgCodec` and `Unicode.h` from OmegaWTK to OmegaCommon

Status: Phase 0 complete — decisions recorded in §11, Phase 1 unblocked.
Scope: `wtk/include/omegaWTK/Media/ImgCodec.h`, `wtk/src/Media/{ImgCodec,JpegCodec,PngCodec,TiffCodec,ImgCodecPriv}.{cpp,h}`, `wtk/include/omegaWTK/Core/Unicode.h`, `wtk/src/Core/Unicode.cpp` — and the consumers in OmegaWTK (Composition, UI, Native), with the goal of making both APIs reusable from AQUA without taking a dependency on OmegaWTK.

---

## 1. Motivation

`BitmapImage` / `loadImageFrom*` and `UniString` are general-purpose utilities that have no real coupling to the OmegaWTK windowing/composition layer:

- `ImgCodec` only depends on `OmegaCommon::AssetBundle`, `OmegaCommon::FS::Path`, `OmegaCommon::HttpClientContext`, and the libpng / libjpeg-turbo / libtiff trio.
- `UniString` only depends on ICU's `ustring.h`.
- AQUA currently has no image-loading or unicode-string primitive of its own. As soon as AQUA's editor or asset pipeline needs either, it would either re-implement them, link OmegaWTK (a cross-direction dependency), or fork the code. None of those are acceptable.

Goal: have a single home for these utilities under the `common/` tree, consumable from both OmegaWTK and AQUA.

## 2. Constraints to respect

These are the things the existing codebase already commits to. The plan must not regress them.

1. **OmegaCommon is currently lightweight.** Its third-party surface today is pcre2 + openssl + libcurl. Folding ICU + libpng + libjpeg-turbo + libtiff into the *same* `OmegaCommon` shared library would force every OmegaCommon consumer (e.g. `omega-wrapgen`, `omega-assetc`, `omega-ebin`, AQUA's BasicGame, all of WTK's submodule libs) to link those four libraries even when they don't need them. That is a real cost — ICU alone is ~30 MB of data tables, and the WTK build already manages it carefully (`OmegaWTK_ThirdPartyInstallNames` rpath fixups on macOS, DLL copying on Windows).
2. **The third-party `add_third_party()` blocks for ICU / libpng / libjpeg-turbo / libtiff currently live in `wtk/CMakeLists.txt`** and contribute to the `OmegaWTK` framework's `EMBEDDED_LIBS`. Moving the source code without moving the build wiring will leave OmegaCommon unable to link.
3. **`OmegaWTK::Media::BitmapImage` is part of OmegaWTK's *public* API surface.** It appears in `Canvas::drawImage`, `CanvasView::drawImage`, `Media::AudioVideoProcessorContext`, the `API.rst` reference, and the `Widget-Stub-Implementation-Plan` / `Canvas-Layer-Exclusivity-Plan` documents. A naïve rename will cascade through everything that uses `Media::BitmapImage` or `UniString` — both directly and through documentation.
4. **`StatusWithObj<T>` is OmegaWTK-only.** Replacing it on the new public surface means picking a Common-side equivalent. `OmegaCommon::Result<T, E>` already exists (`common/include/omega-common/utils.h:780`) and is the natural fit.
5. **The build is dual-tracked.** Both `CMakeLists.txt` and `AUTOM.build` files describe the same targets. Both must be updated in the same change to avoid breaking either build path.

## 3. Architectural shape (decided: Option A)

`ImgCodec` and `UniString` are folded into the existing `OmegaCommon` shared library. Rationale (from Phase 0): most OmegaCommon consumers across the project will end up using one or both of these utilities, so paying once at the OmegaCommon level is preferable to multiplying link targets.

```
common/
  include/
    omega-common/
      common.h, fs.h, ...        (unchanged)
      img.h                       (new — was wtk/include/omegaWTK/Media/ImgCodec.h)
      unicode.h                   (new — was wtk/include/omegaWTK/Core/Unicode.h)
  src/
    img/
      ImgCodec.cpp                (moved from wtk/src/Media)
      JpegCodec.cpp
      PngCodec.cpp
      TiffCodec.cpp
      ImgCodecPriv.h              (moved, stays private)
    unicode/
      UniString.cpp               (moved from wtk/src/Core/Unicode.cpp)
  deps/
    icu/, libpng/, libjpeg-turbo/, libtiff/, zlib/
                                  (fetched via common/AUTOMDEPS — see §6)
```

Linkage rules established in Phase 0:

- **libpng, libjpeg-turbo, libtiff, zlib link statically** into the `OmegaCommon` shared library. They do not appear as separate runtime artefacts; downstream consumers of OmegaCommon get image decode without an extra dylib/DLL.
- **ICU stays SHARED and is embedded alongside `OmegaCommon`** (its existing rpath / install-name / DLL-copy fixups carry over from WTK to Common). Statically linking ICU is impractical given its size and the existing build's data-table handling.
- The previous `OmegaWTK` framework `EMBEDDED_LIBS` no longer needs ICU + the image libs once OmegaCommon owns them — see §6.

(Option B — sibling sub-libraries under `common/` — was considered and rejected during Phase 0. Notes preserved in §11.)


## 4. Public API translation

| Old (OmegaWTK) | New (OmegaCommon) |
|---|---|
| `OmegaWTK::Media::BitmapImage` | `OmegaCommon::Img::BitmapImage` |
| `OmegaWTK::Media::ImgProfile` | `OmegaCommon::Img::Profile` |
| `OmegaWTK::Media::ImgByte` | `OmegaCommon::Img::Byte` |
| `OmegaWTK::Media::ImgHeader` | `OmegaCommon::Img::Header` |
| `BitmapImage::Format` (PNG/TIFF/JPEG) | `OmegaCommon::Img::Format` |
| `BitmapImage::ColorFormat` | `OmegaCommon::Img::ColorFormat` |
| `BitmapImage::AlphaFormat` | `OmegaCommon::Img::AlphaFormat` |
| `loadImageFromFile(path)` returning `StatusWithObj<BitmapImage>` | `OmegaCommon::Img::loadFromFile(path)` returning `OmegaCommon::Result<BitmapImage, std::string>` |
| `loadImageFromAssets(bundle, path)` | `OmegaCommon::Img::loadFromAssets(bundle, path)` |
| `loadImageFromBuffer(data, size, fmt)` | `OmegaCommon::Img::loadFromBuffer(data, size, fmt)` |
| `loadImageFromURL(url, fmt)` | `OmegaCommon::Img::loadFromURL(url, fmt)` |
| `OmegaWTK::UniString` | `OmegaCommon::UniString` |
| `OmegaWTK::UnicodeChar` (= `char16_t`) | `OmegaCommon::UnicodeChar` |
| `OmegaWTK::Unicode32Char` (= `char32_t`) | `OmegaCommon::Unicode32Char` |

Notes on the translation:

- `OPT_PARAM` (= `OmegaWTK::Core::Option`, an `unsigned char`) is used as the underlying type for the enums in the old header. The new header should switch to plain `std::uint8_t` rather than introduce a Common-side replica of the WTK macro. The macro existed for terseness, not invariant — replacing it with a direct integer type is a one-line change per enum and removes a hidden dependency on WTK's `Core.h`.
- `Core::IStream` (= `std::istream`) and `Core::UniquePtr<T>` (= `std::unique_ptr<T>`) are used inside `ImgCodec.cpp` / `ImgCodecPriv.h`. These aliases exist only for ergonomics in WTK code — the moved code should use the standard-library types directly. (No new typedef needed in OmegaCommon.)
- `StatusWithObj<T>` → `OmegaCommon::Result<T, std::string>`. Call sites change from `if (!result) ... result.getError() ... result.getValue()` to `result.isOk() / result.error() / result.value()` — small, mechanical.

## 5. Source-tree moves

```
wtk/include/omegaWTK/Media/ImgCodec.h
    → common/include/omega-common/img.h              (rewritten header, new namespace)

wtk/src/Media/ImgCodec.cpp
wtk/src/Media/JpegCodec.cpp
wtk/src/Media/PngCodec.cpp
wtk/src/Media/TiffCodec.cpp
wtk/src/Media/ImgCodecPriv.h
    → common/src/img/*.{cpp,h}

wtk/include/omegaWTK/Core/Unicode.h
    → common/include/omega-common/unicode.h

wtk/src/Core/Unicode.cpp
    → common/src/unicode/UniString.cpp
```

`wtk/include/omegaWTK/Media/ImgCodec.h` and `wtk/include/omegaWTK/Core/Unicode.h` should not be deleted in the same change — see §9.

`wtk/src/Media/{avf,wmf,ffmpeg}/` (audio/video backends, `MediaPlaybackSession`, `Audio.h`, `Video.h`, `MediaIO.h`, `AudioVideoProcessorContext.h`) **stay in OmegaWTK** for now and are slated to migrate to a future `OmegaVA` module under `{repo_root}/video/` (out of scope for this plan — see §12). `AudioVideoProcessorContext.h` includes `Media/ImgCodec.h` for video frame -> bitmap conversion; it should be updated to include `<omega-common/img.h>` but stay in WTK.

## 6. Build-system changes

### 6a. Move AUTOMDEPS entries

Migrate five entries from `wtk/AUTOMDEPS` to `common/AUTOMDEPS`, alongside the existing `pcre2` / `openssl` / `rapidjson` blocks (`common/AUTOMDEPS:10–47`):

- `libpng` (git, `glennrp/libpng`)
- `libjpeg-turbo` (git, `libjpeg-turbo/libjpeg-turbo`, ref `2.0.x`)
- `libtiff` (gitlab, `libtiff/libtiff`)
- `zlib` (git, `madler/zlib`) — transitive dep of png / jpeg / tiff; fetched in common because the static archives reference it.
- `icu` (git, `unicode-org/icu`, with the `version_source` block describing the `maint/maint-{major}` branch pattern)

`wtk/AUTOMDEPS` keeps `libxml2` (only `OmegaWTK_Core` consumes it) and `ffmpeg` (will follow `OmegaVA` when video moves out — see §12).

The existing `dest` paths use `$(third_party_dest)/<name>/code` (relative to the project root). After the move, the deps land at `common/deps/<name>/code/` — matching the directory layout sketched in §3. No structural changes to the AUTOMDEPS schema; just relocation of the JSON entries.

### 6b. Move the `add_third_party()` blocks (CMake)

The `add_third_party(NAME icu ...)`, `... libpng ...`, `... libjpeg-turbo ...`, `... libtiff ...`, `... zlib ...` blocks at `wtk/CMakeLists.txt:92–311` move to `common/CMakeLists.txt`, after the existing `pcre2` / `openssl` blocks. Two adjustments to the moved blocks:

- **libpng / libjpeg-turbo / libtiff / zlib switch from `EXPORT_SHARED_LIBS` to `EXPORT_STATIC_LIBS`.** Pass `-DBUILD_SHARED_LIBS=OFF` in their `CMAKE_BUILD_ARGS` (mirroring how `pcre2` is already configured at `common/CMakeLists.txt:79`). The platform-specific `*_EXPORT` and `*_DLL` variables in `wtk/CMakeLists.txt:202–311` collapse — there are no more dylibs / DLLs / install-name fixups for these four deps. The macOS rpath/install-name dependencies in `OmegaWTK_ThirdPartyInstallNames` (`wtk/CMakeLists.txt:553–569`) for `libpng.dylib`, `libtiff.dylib`, and the `libz.1.dylib` reset rules drop out.
- **icu stays as `EXPORT_SHARED_LIBS`.** The existing macOS install-name fixups (`wtk/CMakeLists.txt:172–180`) and Windows DLL deploy logic (`wtk/CMakeLists.txt:107–126`) move with the block to `common/CMakeLists.txt`. ICU becomes part of OmegaCommon's `EMBEDDED_LIBS` (see §6c).

After the move, `wtk/CMakeLists.txt` no longer calls `add_third_party()` for these five. The `add_dependencies(ThirdParty icu zlib libpng libjpeg-turbo libtiff libxml2)` line at `wtk/CMakeLists.txt:317` collapses to `add_dependencies(ThirdParty libxml2)`, with a sibling `add_dependencies(OmegaCommonThirdParty icu zlib libpng libjpeg-turbo libtiff)` aggregate added to `common/CMakeLists.txt`.

### 6c. OmegaCommon target updates

In `common/CMakeLists.txt`:

- Extend `COMMON_SRCS`: change the existing `file(GLOB COMMON_SRCS CONFIGURE_DEPENDS "src/*.cpp")` (line 21) to also pick up `src/img/*.cpp` and `src/unicode/*.cpp`. Or split into a small set:
  ```cmake
  file(GLOB COMMON_SRCS         CONFIGURE_DEPENDS "src/*.cpp")
  file(GLOB COMMON_IMG_SRCS     CONFIGURE_DEPENDS "src/img/*.cpp")
  file(GLOB COMMON_UNICODE_SRCS CONFIGURE_DEPENDS "src/unicode/*.cpp")
  list(APPEND COMMON_SRCS ${COMMON_IMG_SRCS} ${COMMON_UNICODE_SRCS})
  ```
- Static deps wired into the existing `OmegaCommon` module:
  ```cmake
  target_link_libraries("OmegaCommon" PRIVATE pcre2-8 ssl crypto
      png turbojpeg tiff z          # static — folded into libOmegaCommon
      icuuc icudata icui18n)        # shared — embedded alongside, see EMBEDDED_LIBS
  ```
- `add_omega_graphics_module("OmegaCommon" SHARED ...)` (line 162) gains an `EMBEDDED_LIBS` argument so the ICU dylibs/DLLs ship next to `libOmegaCommon`:
  ```cmake
  add_omega_graphics_module("OmegaCommon" SHARED
      SOURCES ${COMMON_SRCS}
      HEADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include"
      EMBEDDED_LIBS ${ICU_EXPORT})
  add_dependencies("OmegaCommon" OmegaCommonThirdParty)
  ```
- The macOS install-name aggregator currently called `OmegaWTK_ThirdPartyInstallNames` (`wtk/CMakeLists.txt:555–569`) gets a sibling `OmegaCommon_ThirdPartyInstallNames` for the ICU rpath fixups; WTK's aggregator drops the ICU entries.
- No new export macro is needed — `OMEGACOMMON_EXPORT` (`common/include/omega-common/utils.h:32–38`) covers the new `img.h` and `unicode.h` headers.

### 6d. WTK linkage updates

In `wtk/CMakeLists.txt`:

- `OmegaWTK_Core` already links `OmegaCommon` (line 423). Drop the direct `icuuc icudata icui18n` from its `target_link_libraries` — they come transitively now via `OmegaCommon`. `xml2` stays.
- `OmegaWTK_Media` drops the direct `png turbojpeg tiff` links (line 451) — they come from `OmegaCommon`. After this and §12's OmegaVA move-out, `OmegaWTK_Media` may eventually be deleted entirely.
- `OmegaWTK_Composition`, `OmegaWTK_UI`, `OmegaWTK_Widgets`, `OmegaWTK_Native` already link `OmegaCommon`; nothing changes.
- The `OmegaWTK` framework's `EMBEDDED_LIBS` (`wtk/CMakeLists.txt:527`) drops `${ICU_EXPORT} ${LIBPNG_EXPORT} ${LIBJPEGTURBO_EXPORT} ${LIBTIFF_EXPORT} ${ZLIB_EXPORT}`. The image libs are gone (now static in OmegaCommon). ICU is shipped by OmegaCommon's `EMBEDDED_LIBS`. Anything that loads OmegaWTK loads OmegaCommon, so the dylibs still resolve at runtime.
- `wtk/CMakeLists.txt:584` — `target_link_libraries("OmegaWTK" PUBLIC "OmegaCommon" icuuc icudata icui18n xml2 png turbojpeg tiff)` collapses to `... PUBLIC "OmegaCommon" xml2`.

### 6e. AUTOM.build

Mirror the CMake changes in `common/AUTOM.build` and `wtk/AUTOM.build`:

- `common/AUTOM.build` — extend the `omega_common_lib` `Shared` target's source list to include `./src/img/*.cpp` and `./src/unicode/*.cpp`. Add the static link references for png / turbojpeg / tiff / z and shared-link references for icu*. (The current AUTOM.build at `common/AUTOM.build:1–35` does not specify image/icu deps; this is the first time they appear.)
- `wtk/AUTOM.build` — drop the corresponding image / ICU dep references; keep libxml2 / ffmpeg.

## 7. Consumer updates inside OmegaWTK

After the moves and the compatibility shim (§9) is in place, update WTK's own code to use the new headers and namespaces:

- `wtk/include/omegaWTK/Composition/Canvas.h` — `Media::BitmapImage` → `OmegaCommon::Img::BitmapImage`. Add `#include <omega-common/img.h>`. Adjust the `VISUAL_COMMAND_ARGS_CHECK` template specialisation arg list.
- `wtk/include/omegaWTK/Composition/FontEngine.h` — `OmegaWTK::UniString` → `OmegaCommon::UniString`.
- `wtk/include/omegaWTK/Native/NativeEvent.h` — `OmegaWTK::Unicode32Char` → `OmegaCommon::Unicode32Char`.
- `wtk/include/omegaWTK/UI/CanvasView.h` — both `Media::BitmapImage` and `UniString`.
- `wtk/include/omegaWTK/Media/AudioVideoProcessorContext.h` — re-include the new header.
- Composition backends:
  - `wtk/src/Composition/Canvas.cpp`
  - `wtk/src/Composition/backend/dx/DWriteFontEngine.cpp`
  - `wtk/src/Composition/backend/mtl/CTFontEngine.mm`
  - `wtk/src/Composition/backend/vk/HarfbuzzFontEngine.cpp`
- `wtk/src/Media/ImgCodec.cpp` and friends are moved, not edited in place — but the *call sites* inside Composition/UI that read `BitmapImage::data`, `BitmapImage::header`, etc. need namespace updates only (field layout is unchanged).

The `StatusWithObj<BitmapImage>` → `Result<BitmapImage, std::string>` change ripples to wherever `loadImageFrom*` is called in WTK. Grep target: `loadImageFrom`. Confirmed call sites to update at minimum: `wtk/src/UI/App.cpp` (asset bundle auto-load) and any widget loading images. The WTK app tests that depend on `StatusWithObj<BitmapImage>` will be updated as part of this phase (acknowledged in Phase 0).

## 8. Documentation updates

- `wtk/docs/API.rst` — every `Media::BitmapImage` reference (lines 975, 1072, 1443) and every `UniString` reference (1073–1074, 1434, 1439). Replace with the OmegaCommon names and add a "moved in version X" note.
- `wtk/docs/ImgCodec-API-Extension-Proposal.md` — this proposal predates the move; either supersede it ("see Common-ImgCodec-Unicode-Refactor-Plan.md, then re-do extension on the Common-side API") or rebase its diff onto the new namespace.
- `wtk/docs/Media-API-Completion-Plan.md` — flag the image part as moved.
- `wtk/docs/Composition-Extension-Plan.md` — `UniString` references update with the namespace.
- `wtk/docs/Widget-Stub-Implementation-Plan.md`, `wtk/docs/done/Canvas-Layer-Exclusivity-Plan.md`, `wtk/docs/stale/Brush-API-Extension-Proposal.md`, `wtk/docs/stale/Canvas-API-Extension-Proposal.md` — update references; `done/` and `stale/` may be left as historical record with a note at the top.
- `wtk/docs/ImgCodec-API-Extension-Proposal.md` — relocate to `common/docs/` (per Phase 0). The proposal predates this move; rebase its diff onto the new `OmegaCommon::Img` surface as a follow-up there.
- `common/README.md` — document the new `Img::` and `UniString` APIs and the new third-party deps (libpng/libjpeg-turbo/libtiff/zlib statically linked, ICU embedded).
- `common/Doxyfile` — make sure `include/omega-common/img.h` and `include/omega-common/unicode.h` are picked up by the existing input pattern.
- `AGENTS.md` (root) — note that `OmegaCommon` now owns image decode and ICU-backed unicode utilities.

## 9. Compatibility shim (transient)

Cross-cutting renames are easy to get wrong, and the "old" headers appear in user-written WTK app code (`#include <omegaWTK/Media/ImgCodec.h>` is part of the documented public API). To keep the build green during Phases 2–4, leave thin shim headers in place:

`wtk/include/omegaWTK/Media/ImgCodec.h` becomes:

```cpp
#ifndef OMEGAWTK_MEDIA_IMGCODEC_H
#define OMEGAWTK_MEDIA_IMGCODEC_H
// Deprecated: this header has moved to <omega-common/img.h>.
// Update your includes; this shim will be removed.
#include <omega-common/img.h>
namespace OmegaWTK { namespace Media {
    using BitmapImage = OmegaCommon::Img::BitmapImage;
    using ImgProfile  = OmegaCommon::Img::Profile;
    using ImgByte     = OmegaCommon::Img::Byte;
    using ImgHeader   = OmegaCommon::Img::Header;
    // loadImageFrom* free functions: inline forwarders that translate
    // OmegaCommon::Result back into StatusWithObj while WTK consumers migrate.
}}
#endif
```

`wtk/include/omegaWTK/Core/Unicode.h` becomes a similar shim.

Mark both with a `[[deprecated]]` `#pragma message` so consumers get a warning at compile time.

Shim lifetime is **not** tied to a specific OmegaWTK version (the version numbers in `wtk/CMakeLists.txt:3` and `common/CMakeLists.txt:3` are stale per Phase 0). Concrete sequencing: keep the shims through Phase 4 (WTK internal flip); decide whether to remove or keep them in Phase 6 once the WTK + AQUA call sites have all moved over. Removal can be its own commit, separate from this refactor.

## 10. Phased plan

The change is mechanical but wide. Phasing it lets the build stay green at each step.

**Phase 0 — preparation. ✅ Complete.** Decisions recorded in §11.

**Phase 1a — structural move only. ✅ Complete.** AUTOMDEPS entries (libpng / libjpeg-turbo / libtiff / zlib / icu) moved from `wtk/AUTOMDEPS` to `common/AUTOMDEPS`. Corresponding `add_third_party()` blocks moved from `wtk/CMakeLists.txt` to `common/CMakeLists.txt`. macOS install-name fixups (set_library_install_name / reset_library_dependent_name / add_library_rpath) for ICU + libpng + libtiff also moved — their custom-target names are global, so wtk's `OmegaWTK_ThirdPartyInstallNames` aggregator continues to depend on them by name. The `ICU_EXPORT`, `ICU_DLLS`, `ICU_VERSION`, `LIBPNG_EXPORT/_DLL`, `LIBJPEGTURBO_EXPORT/_DLL`, `LIBTIFF_EXPORT/_DLL`, `ZLIB_EXPORT/_DLL`, and `OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR` variables are promoted to `CACHE INTERNAL` so wtk sees them across the subdir boundary. wtk's libxml2 build now references `${OMEGACOMMON_THIRD_PARTY_OUTPUT_DIR}` for its zlib `CMAKE_PREFIX_PATH` and ICU header `-I` flag. Image deps remain SHARED — the static switch is Phase 1b. Existing checkouts under `wtk/deps/{icu,libpng,libjpeg-turbo,libtiff,zlib}/` are now orphaned; re-run AUTOMDEPS to populate `common/deps/...`.

**Phase 1b — static switch. ✅ Complete.** libpng / libjpeg-turbo / libtiff / zlib flipped from `EXPORT_SHARED_LIBS` to `EXPORT_STATIC_LIBS`. Per-project disable-shared flags applied: zlib + libtiff take `-DBUILD_SHARED_LIBS=OFF`; libpng uses native `-DPNG_SHARED=OFF -DPNG_STATIC=ON`; libjpeg-turbo uses native `-DENABLE_SHARED=OFF -DENABLE_STATIC=ON`. Verified per-platform static archive names: `lib/libz.a` + `lib/zlibstatic.lib`, `lib/libpng.a` (UNIX symlink → `libpng16.a`) + `lib/libpng16_static.lib`, `lib/libturbojpeg.a` + `lib/turbojpeg-static.lib`, `lib/libtiff.a` + `lib/tiff.lib`. The four image libs dropped from `OmegaWTK`'s `EMBEDDED_LIBS`, `THIRD_PARTY_EXPORTS`, and `THIRD_PARTY_DLLS`. The libpng / libtiff dylib install-name and rpath fixups dropped from `OmegaWTK_ThirdPartyInstallNames`; ICU's fixups remain. The four `*_EXPORT` / `*_DLL` `CACHE INTERNAL` variables that wtk no longer references were also dropped from common. Image symbols are now baked into the OmegaWTK shared library at link time via OmegaWTK_Media's static link of `png`/`turbojpeg`/`tiff`/`z`.

**Phase 2 — Unicode in Common. ✅ Complete.** Added `common/include/omega-common/unicode.h` (public API: `OmegaCommon::UniString`, `UnicodeChar`, `Unicode32Char` — exports via `OMEGACOMMON_EXPORT`, no ICU types in the public surface) and `common/src/unicode/UniString.cpp` (verbatim port of the WTK impl into the `OmegaCommon` namespace; uses ICU's `u_strFromUTF8` / `u_strFromUTF32` internally). Extended `common/CMakeLists.txt` `COMMON_SRCS` to glob `src/unicode/*.cpp`. `OmegaCommon` now `PRIVATE`-links `icuuc icudata icui18n` and has `add_dependencies("OmegaCommon" icu)` so ICU builds before OmegaCommon. `wtk/include/omegaWTK/Core/Unicode.h` reduced to a shim — `#include <omega-common/unicode.h>` plus `using UniString = OmegaCommon::UniString;` (etc.) in namespace `OmegaWTK`. `wtk/src/Core/Unicode.cpp` deleted. Every existing wtk caller (Composition backends, FontEngine, NativeEvent, CanvasView, Canvas.cpp) keeps compiling unchanged through the alias.

**ICU embedding deferred.** `add_omega_graphics_module(SHARED ...)` does not consume `EMBEDDED_LIBS` (only the FRAMEWORK path does — see `cmake/OmegaGraphicsSuite.cmake:752-754`). WTK continues to own ICU embedding via its framework's `EMBEDDED_LIBS ${ICU_EXPORT}`. When AQUA-only deployment of OmegaCommon becomes a concern (Phase 5+), either extend the helper to support `EMBEDDED_LIBS` for non-FRAMEWORK SHARED on Apple/Win, or hand-roll the dylib/DLL copy alongside `libOmegaCommon`. Tracked as a Phase 5 prerequisite.

**Phase 3 — Img in Common.** Same shape as Phase 2, for image codecs. Replace `StatusWithObj` returns with `OmegaCommon::Result`. Wire libpng / libjpeg-turbo / libtiff / zlib into `OmegaCommon` as static deps. WTK's `Media/ImgCodec.h` becomes a shim with inline `StatusWithObj` ↔ `Result` adapters. Build, run WTK tests, render a few test images end-to-end.

**Phase 4 — flip WTK internals to the new headers.** Replace `Media::BitmapImage` / `UniString` references inside `wtk/src/` and `wtk/include/` with the new names. Update the WTK app tests for the `StatusWithObj` → `Result` API change. Drop the now-redundant `icuuc/icudata/icui18n/png/turbojpeg/tiff` direct links and `EMBEDDED_LIBS` entries from `wtk/CMakeLists.txt` per §6d. Run all WTK tests + the BasicGame smoke test in AQUA.

**Phase 5 — first AQUA consumer.** Pick one AQUA call site that currently rolls its own image or string handling (or add a new test) and have it `#include <omega-common/img.h>` directly. This validates that AQUA can use the common library without dragging WTK in. (Sanity check: the linker should not pull `OmegaWTK_*` into the AQUA test binary.)

**Phase 6 — shim removal (decide later).** Once WTK + AQUA are fully migrated, delete the `wtk/include/omegaWTK/Media/ImgCodec.h` and `wtk/include/omegaWTK/Core/Unicode.h` shims and the `StatusWithObj` adapter. Timing not committed in advance — see §9.

Each phase is independently revertable. Phase 1 is the riskiest (build-system) and the cheapest to validate (`cmake --build` on each platform); phases 2–4 are mechanical.

## 11. Decisions recorded (Phase 0)

1. **Architecture: Option A.** Fold into `OmegaCommon` directly rather than spinning sibling sub-libraries. Rationale: most libraries across the project will use these features, so paying once at the OmegaCommon level is cheaper than multiplying targets.
2. **Namespace: `OmegaCommon::Img`.** Matches the existing `Img*` prefix style in the WTK source.
3. **Third-party source layout: AUTOMDEPS protocol.** All deps follow the JSON protocol used in `common/AUTOMDEPS`; libpng / libjpeg-turbo / libtiff / zlib / icu entries move from `wtk/AUTOMDEPS` to `common/AUTOMDEPS` next to pcre2 / openssl / rapidjson, fetched into `common/deps/<name>/code/`.
4. **Embed list: image deps STATIC into `OmegaCommon`; ICU stays SHARED and is embedded by `OmegaCommon`.** The WTK framework no longer ships its own copies of these libraries. ICU's existing rpath / install-name / DLL-copy fixups migrate from WTK's CMake to OmegaCommon's.
5. **`StatusWithObj` migration: accepted.** WTK app tests will be updated as part of Phase 4 to consume `OmegaCommon::Result<BitmapImage, std::string>`.
6. **Shim removal not version-pinned.** Project version numbers are stale and will be re-evaluated separately. Shim survival is gated on consumer migration, not on a release number — see §9.
7. **`OPT_PARAM` / `Core::Option`: drop on the moved enums.** New `omega-common/img.h` uses `std::uint8_t` directly. No new alias introduced in OmegaCommon.

## 12. Out of scope

- The `Media/Audio*`, `Media/Video*`, `MediaPlaybackSession`, `MediaIO.h`, `AudioVideoProcessorContext` and the platform backends under `wtk/src/Media/{avf,wmf,ffmpeg}/` stay in OmegaWTK for now. These will migrate to a future **`OmegaVA` module under `{repo_root}/video/`** (own AUTOMDEPS, ffmpeg lives there). Tracked separately; not blocking this refactor.
- The libxml2 dependency in `OmegaWTK_Core` and its AUTOMDEPS entry — stays in `wtk/AUTOMDEPS` and `wtk/CMakeLists.txt`.
- The ffmpeg AUTOMDEPS entry — stays in `wtk/AUTOMDEPS` until the OmegaVA migration above lands.
- `loadAssetFile()` (declared in `wtk/include/omegaWTK/Core/Core.h:99`) — appears unused; track separately.
- The `ImgCodec-API-Extension-Proposal.md` improvements (better codec abstraction, better error reporting). The proposal document **moves to `common/docs/`** as part of this refactor (per Phase 0), and the API extension itself becomes a follow-up against the new `OmegaCommon::Img` surface.
