# omega-assetc Extension Plan

## Current State

`omega-assetc` is a minimal CLI tool that packs raw files into a flat binary bundle (`.omxa`). At runtime, `AssetLibrary` in OmegaCommon reads the bundle into a static `Map<String, AssetBuffer>` of raw `void*` pointers. Consumers like WTK's `ImgCodec` look up assets by path string and reinterpret the buffer manually.

### What exists today

| Component | Location | Status |
|-----------|----------|--------|
| Binary format structs | `common/assetc/assetc.h` | `AssetsFileHeader` (asset count) + `AssetsFileEntry` (name length, data length) |
| Compiler CLI | `common/assetc/main.cpp` | Packs input files sequentially. Commented-out SHA-256 + AES-128-CBC signing code. Hardcoded key/IV. |
| Runtime loader | `common/src/assets.cpp` | Reads `.omxa`, stores raw buffers in a global static map. No deallocation. |
| Runtime API | `common/include/omega-common/assets.h` | `AssetLibrary` with `loadAssetFile` and a public `assets_res` map of `{size_t filesize, void *data}`. |
| WTK consumer | `wtk/src/Media/ImgCodec.cpp` | `loadImageFromAssets()` indexes into `assets_res` by path string, wraps the buffer as an `istream`. |
| WTK build integration | `wtk/cmake/OmegaWTKApp.cmake` | `ASSET_DIR` parameter exists but is unused. |
| Embed-binary tool | `common/embedbin/embedbin.cpp` | Separate tool (`omega-ebin`) that converts binary files to C char arrays for compile-time embedding. |

### Problems

1. **No asset metadata.** The format stores name + blob. There is no content type, compression flag, dimensions, or version information. Consumers must guess what kind of data they received.
2. **No integrity checking.** The commented-out signing code used a hardcoded AES key and was never finished. There is no checksum, no signature verification, no tamper detection.
3. **No encryption.** The commented-out code was AES-128-CBC (no authentication, no proper key management). Assets ship in cleartext.
4. **Memory management.** `AssetBuffer.data` is a raw `new[]` allocation never freed. No RAII, no shared ownership.
5. **Global mutable state.** `assets_res` is a public static map. Anyone can mutate it. No thread safety.
6. **Flat namespace.** Assets are keyed by their original filesystem path, which is fragile and environment-dependent.
7. **No build integration.** The `OmegaWTKApp` CMake function accepts `ASSET_DIR` but never invokes `omega-assetc`.
8. **No streaming.** The entire bundle is read into memory at once. Large bundles (video, high-res textures) will spike memory.
9. **No relationship to `omega-ebin`.** Two separate embedding strategies with no shared infrastructure.

---

## Design Goals

- **Single bundle format** for images, fonts, shaders, data files, and any future asset type.
- **Compile-time processing** where the tool can validate, compress, and optionally encrypt assets before they ship.
- **Runtime loading** that is safe (RAII, typed access), efficient (lazy/streaming), and thread-safe.
- **Encryption and integrity** using OmegaCommon's crypto module (Phase 5 / 5b) rather than raw OpenSSL calls.
- **Build integration** so WTK and AQUA apps automatically compile their asset directories into bundles.
- **Backward compatible** with the existing `.omxa` format (read old bundles, write new format).

---

## Phase A: Binary Format v2

### A.1 Bundle Layout

```
[BundleHeader]
[AssetEntry] * entry_count
[String Table]         -- packed, null-terminated asset names
[Data Region]          -- asset payloads (possibly compressed / encrypted)
```

### A.2 BundleHeader

```cpp
struct BundleHeader {
    uint8_t  magic[4];       // "OMXA"
    uint16_t version;        // 2
    uint16_t flags;          // BundleFlags bitfield
    uint32_t entryCount;
    uint32_t stringTableSize;
    uint64_t dataRegionOffset;
    uint64_t dataRegionSize;
    uint8_t  bundleHash[32]; // SHA-256 over entries + string table + data region
};
```

### A.3 BundleFlags

```cpp
enum BundleFlags : uint16_t {
    None        = 0,
    Compressed  = 1 << 0,  // data region uses per-entry compression
    Encrypted   = 1 << 1,  // data region uses per-entry AES-256-GCM encryption
    Signed      = 1 << 2,  // bundleHash is present and verified on load
};
```

### A.4 AssetEntry

```cpp
struct AssetEntry {
    uint32_t nameOffset;     // offset into string table
    uint16_t nameLength;
    uint16_t assetType;      // AssetType enum
    uint64_t dataOffset;     // offset into data region
    uint64_t rawSize;        // uncompressed size
    uint64_t storedSize;     // size on disk (may differ if compressed/encrypted)
    uint32_t flags;          // per-entry flags (compressed, encrypted)
    uint8_t  entryHash[32];  // SHA-256 of raw (uncompressed) content
};
```

### A.5 AssetType

```cpp
enum class AssetType : uint16_t {
    Raw       = 0,   // opaque blob
    Image     = 1,   // PNG, JPEG, TIFF, BMP
    Font      = 2,   // TTF, OTF
    Shader    = 3,   // compiled shader library (.omegasllib)
    Text      = 4,   // plain text, JSON, XML, config
    Audio     = 5,   // WAV, etc.
    Binary    = 6,   // application-specific binary data
};
```

Asset type is assigned automatically by file extension during compilation, or overridden via CLI flag.

### A.6 Backward Compatibility

The loader checks `magic` and `version`. If the file starts with the old format (no magic bytes, just a raw `unsigned asset_count`), it falls back to the v1 reader. New bundles always write v2.

---

## Phase B: Compiler Rewrite

Rewrite `omega-assetc` as a proper asset pipeline tool.

### B.1 CLI Interface

```
omega-assetc [options] inputs...

Options:
  --output, -o <file>           Output bundle path (required)
  --app-id <id>                 Application identifier (used as salt for signing)
  --compress                    Enable zlib compression for eligible assets
  --encrypt                     Enable AES-256-GCM encryption
  --key-file <path>             Encryption key file (32 bytes, or derived via HKDF from passphrase)
  --key-passphrase              Derive encryption key from passphrase (prompted or via env var)
  --sign                        Compute and embed integrity hashes
  --type <name>=<type>          Override asset type for a specific file
  --strip-prefix <prefix>       Strip leading path prefix from asset names
  --manifest <file>             Read asset list from a manifest file instead of CLI args
  --legacy                      Write v1 format (no metadata, no compression, no encryption)
  --verbose, -v                 Print per-asset details during compilation
  --help, -h                    Show help
```

### B.2 Pipeline Steps

For each input file:

1. **Identify** asset type from file extension (or `--type` override).
2. **Read** the raw file content.
3. **Hash** the raw content (SHA-256) for integrity.
4. **Compress** (if `--compress` and asset type is eligible). Use zlib deflate. Store both `rawSize` and `storedSize`.
5. **Encrypt** (if `--encrypt`). AES-256-GCM via OmegaCommon crypto. Per-entry nonce derived from entry index + bundle salt. Authentication tag stored alongside ciphertext.
6. **Write** the entry to the data region.

After all entries:

7. **Compute** the bundle-level hash over the entire content (entries + string table + data region).
8. **Write** the header with final offsets and hash.

### B.3 Manifest File

For projects with many assets, a manifest file avoids long CLI invocations:

```
# comments
images/icon.png
images/splash.png
fonts/main.ttf         type=Font
data/config.json       type=Text
shaders/compositor.omegasllib  type=Shader
```

### B.4 Encryption Design

- Key material is never stored in the bundle. The bundle only contains encrypted ciphertext + per-entry GCM authentication tags.
- Key can be provided as a raw 32-byte file, or derived from a passphrase using HKDF (Phase 5b) with the `--app-id` as salt.
- Per-entry nonces are deterministic: `HKDF-Expand(bundleKey, entryIndex || entryName, 12)`. This allows decryption of individual entries without reading the whole bundle.
- At runtime, the application provides the key to the asset loader. No key = no access to encrypted assets.

### B.5 Dependencies

- OmegaCommon crypto module (Phase 5 for hashing, Phase 5b for AES-GCM and HKDF).
- zlib for compression (already available as a third-party dep in WTK; could be promoted to OmegaCommon or made optional).

---

## Phase C: Runtime Loader Rewrite

Replace the current `AssetLibrary` with a safe, typed runtime API.

### C.1 Public API

```cpp
namespace OmegaCommon {

    enum class AssetType : uint16_t { /* same as format */ };

    struct AssetInfo {
        String name;
        AssetType type;
        size_t rawSize;
    };

    class OMEGACOMMON_EXPORT AssetBundle {
        struct Impl;
        Impl *impl = nullptr;

        explicit AssetBundle(Impl *p);
    public:
        AssetBundle(const AssetBundle &) = delete;
        AssetBundle & operator=(const AssetBundle &) = delete;
        AssetBundle(AssetBundle && other) noexcept;
        AssetBundle & operator=(AssetBundle && other) noexcept;
        ~AssetBundle();

        /// Open a bundle file. Reads the header and entry table only.
        /// Asset data is loaded on demand.
        static Result<AssetBundle, String> open(FS::Path path);

        /// Open an encrypted bundle. Key must be exactly 32 bytes.
        static Result<AssetBundle, String> open(FS::Path path, ArrayRef<std::uint8_t> key);

        /// Query
        size_t entryCount() const;
        Optional<AssetInfo> info(StrRef name) const;
        bool contains(StrRef name) const;
        Vector<AssetInfo> entries() const;

        /// Load asset data by name. Decompresses and decrypts as needed.
        /// Returns the raw asset bytes. Verifies entry hash on load.
        Result<Vector<std::uint8_t>, String> load(StrRef name) const;

        /// Load asset data as a string (convenience for Text assets).
        Result<String, String> loadText(StrRef name) const;
    };

}
```

### C.2 Design Decisions

- **Lazy loading.** `open()` reads only the header + entry table. Asset data is read from disk on `load()`. This keeps memory usage proportional to what the application actually uses.
- **Move-only.** `AssetBundle` owns the file handle and entry index. No shared state.
- **No global map.** Each bundle is an independent object. Applications can hold multiple bundles (e.g., base assets + DLC).
- **Thread-safe reads.** Multiple threads can call `load()` concurrently on the same bundle (the implementation uses per-call file reads or memory-mapped regions, no shared cursor).
- **Integrity verification.** Each `load()` call verifies the entry's SHA-256 hash after decompression/decryption. Tampered or corrupted assets produce an error, not silent garbage.

### C.3 Migration From v1

The existing `AssetLibrary` API is kept but deprecated. Its implementation is updated to wrap the new `AssetBundle`:

```cpp
class [[deprecated("Use AssetBundle instead")]] AssetLibrary {
    // loadAssetFile() internally opens an AssetBundle,
    // loads all entries, and populates the legacy map.
};
```

WTK's `loadImageFromAssets()` is updated to accept an `AssetBundle &` parameter (or a global app bundle accessor) instead of reaching into a static map.

---

## Phase D: Build Integration

### D.1 CMake Function

Add an `omega_compile_assets` function to `OmegaGraphicsSuite.cmake`:

```cmake
function(omega_compile_assets)
    cmake_parse_arguments("_ARG" "COMPRESS;SIGN" "TARGET;OUTPUT;APP_ID;MANIFEST" "INPUTS" ${ARGN})

    if(_ARG_MANIFEST)
        set(_INPUTS --manifest "${_ARG_MANIFEST}")
    else()
        set(_INPUTS ${_ARG_INPUTS})
    endif()

    set(_FLAGS)
    if(_ARG_COMPRESS)
        list(APPEND _FLAGS --compress)
    endif()
    if(_ARG_SIGN)
        list(APPEND _FLAGS --sign)
    endif()

    add_custom_command(
        OUTPUT "${_ARG_OUTPUT}"
        COMMAND omega-assetc -o "${_ARG_OUTPUT}" --app-id "${_ARG_APP_ID}" ${_FLAGS} ${_INPUTS}
        DEPENDS omega-assetc ${_ARG_INPUTS}
        COMMENT "Compiling assets to ${_ARG_OUTPUT}")

    add_custom_target(${_ARG_TARGET}_assets DEPENDS "${_ARG_OUTPUT}")
    add_dependencies(${_ARG_TARGET} ${_ARG_TARGET}_assets)
endfunction()
```

### D.2 OmegaWTKApp Integration

Update `OmegaWTKApp.cmake` to use the `ASSET_DIR` parameter:

```cmake
if(_ARG_ASSET_DIR)
    file(GLOB_RECURSE _ASSET_FILES "${_ARG_ASSET_DIR}/*")
    omega_compile_assets(
        TARGET ${_ARG_NAME}
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/assets.omxa"
        APP_ID "${_ARG_BUNDLE_ID}"
        COMPRESS SIGN
        INPUTS ${_ASSET_FILES})
endif()
```

This means WTK apps that pass `ASSET_DIR` to `OmegaWTKApp()` will automatically get their assets compiled and placed next to the binary.

---

## Phase E: Subsume omega-ebin

`omega-ebin` converts binary files to C/C++ char arrays for compile-time embedding. This is a complementary use case to runtime asset bundles: some assets (small icons, default config) should be compiled directly into the binary rather than loaded from disk.

### E.1 Embed Mode

Add `--embed` mode to `omega-assetc`:

```
omega-assetc --embed --output assets_embedded.h --namespace MyApp inputs...
```

This generates a header file with:

```cpp
// Generated by omega-assetc --embed
namespace MyApp::EmbeddedAssets {
    inline constexpr unsigned char icon_png[] = { 0x89, 0x50, 0x4E, 0x47, ... };
    inline constexpr size_t icon_png_size = 1234;
    // ...
}
```

This replaces `omega-ebin` with a unified tool that handles both runtime bundles and compile-time embedding.

### E.2 CMake Function

```cmake
function(omega_embed_assets)
    cmake_parse_arguments("_ARG" "" "TARGET;OUTPUT;NAMESPACE" "INPUTS" ${ARGN})
    add_custom_command(
        OUTPUT "${_ARG_OUTPUT}"
        COMMAND omega-assetc --embed -o "${_ARG_OUTPUT}" --namespace "${_ARG_NAMESPACE}" ${_ARG_INPUTS}
        DEPENDS omega-assetc ${_ARG_INPUTS}
        COMMENT "Embedding assets to ${_ARG_OUTPUT}")
    target_sources(${_ARG_TARGET} PRIVATE "${_ARG_OUTPUT}")
endfunction()
```

---

## Implementation Order

| Step | Phase | Depends On | Priority |
|------|-------|------------|----------|
| 1 | A: Binary format v2 structs | None | High |
| 2 | C: Runtime loader (`AssetBundle`) | A | High |
| 3 | B: Compiler rewrite (basic: pack + hash + sign) | A | High |
| 4 | D: CMake build integration | B, C | High |
| 5 | B+C: Compression support (zlib) | B, C | Medium |
| 6 | B+C: Encryption support (AES-256-GCM) | Phase 5b, B, C | Medium |
| 7 | E: Embed mode (subsume omega-ebin) | B | Low |
| 8 | Legacy v1 compat reader | C | Low |

Steps 1-4 form a minimum viable asset system. Steps 5-6 add the security/size features. Steps 7-8 are cleanup.

---

## Out Of Scope

- Texture compression (BC/ASTC/ETC2 transcoding). That belongs in GTE or a dedicated texture pipeline.
- Asset hot-reloading during development. Useful but a separate feature.
- Network-based asset delivery / CDN integration.
- DRM or license enforcement. Encryption here is for asset protection against casual extraction, not against determined reverse engineering.

---

## Files

| File | Role |
|------|------|
| `common/assetc/assetc.h` | Shared format structs (v1 + v2) |
| `common/assetc/main.cpp` | Compiler CLI (rewritten) |
| `common/include/omega-common/assets.h` | Runtime API (`AssetBundle` + deprecated `AssetLibrary`) |
| `common/src/assets.cpp` | Runtime implementation |
| `common/CMakeLists.txt` | Build wiring |
| `cmake/OmegaGraphicsSuite.cmake` | `omega_compile_assets` / `omega_embed_assets` functions |
| `wtk/cmake/OmegaWTKApp.cmake` | `ASSET_DIR` integration |
