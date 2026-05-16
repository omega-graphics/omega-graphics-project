# ImgCodec API Extension Proposal

> **Status: Phases 1 + 2 landed.** Namespace is `OmegaCommon::Img::*`,
> result type is `Result<BitmapImage, std::string>`, and `BitmapImage`
> now owns its pixel buffer through a move-only `PixelStorage` wrapper
> with a type-erased deleter (PNG/JPEG use `delete[]`; TIFF uses
> `_TIFFfree`). Probe, decode options, encode, and the capability
> registry remain unstarted. Type names in the code examples below
> still read `OmegaWTK::Media::*`/`StatusWithObj<...>` and need
> translating when each remaining phase is actually implemented.

## Current State

The current image codec layer is implemented in:

- `common/include/omega-common/img.h`
- `common/src/img/ImgCodec.cpp`
- `common/src/img/ImgCodecPriv.h`
- `common/src/img/PngCodec.cpp`
- `common/src/img/JpegCodec.cpp`
- `common/src/img/TiffCodec.cpp`

`common/AUTOMDEPS` already declares the required image dependencies:

- `libpng`
- `libjpeg-turbo`
- `libtiff`
- `zlib`

The build also links `OmegaWTK_Media` against `png`, `turbojpeg`, and `tiff`, so finishing the existing API does not require a new dependency. The extension should first make PNG, JPEG, and TIFF complete and safe before adding new formats.

### What Works

- `loadImageFromFile()` selects a codec by file extension.
- `loadImageFromAssets()` loads from an `AssetBundle` or the legacy `AssetLibrary`.
- `loadImageFromBuffer()` decodes caller-provided encoded bytes when a format is supplied.
- `loadImageFromURL()` fetches a URL and decodes the response body.
- PNG has the most complete implementation and reads header, gamma, color profile, transparency, and several optional chunks.
- TIFF can decode to RGBA through `TIFFReadRGBAImage`.
- JPEG uses `libjpeg-turbo` as the intended backend.

### Gaps

- `BitmapImage` owns `ImgByte *data` but has no destructor, deleter, allocator tag, copy/move policy, or explicit ownership contract.
- The public API exposes decoded pixels but not the encoded image metadata, codec capabilities, or decode options.
- JPEG does not currently fill `BitmapImage::header`, `profile`, gamma, alpha, or color metadata.
- JPEG allocates an encoded input buffer but does not read the stream into it before calling TurboJPEG.
- `loadImageFromBuffer()` assumes codec construction always succeeds and calls `codec->readToStorage()` without a null check.
- Format detection is extension-based for files/assets and caller-supplied for buffers/URLs. It should use magic-byte sniffing first.
- Decode errors are collapsed into generic strings. The caller cannot distinguish unsupported format, corrupt data, I/O failure, allocation failure, or unsupported pixel conversion.
- There is no image encoding API. The name `ImgCodec` implies codec support, but the public API is decode-only.
- PNG logs metadata to `std::cout`; the codec should return metadata through the API instead.
- Typoed enum names (`Pallete`, `Premultipled`, `Ingore`) are part of the public API and should be replaced through a compatibility phase, not silently reused.

## Design References

The extension should follow the same broad patterns used by mature image layers:

- **WIC / CoreGraphics / Skia**: separate image sniffing, metadata, decode options, decoded pixel storage, and encode options.
- **stb_image / SDL_image**: keep simple one-call loading available for application code.
- **libpng / libjpeg-turbo / libtiff**: keep codec-specific implementation private and expose only normalized OmegaWTK image types.

The result should preserve the current convenient `loadImageFrom*()` calls while adding a real codec API below them.

## Goals

- Make decoded image ownership deterministic and leak-free.
- Normalize decoded pixels so Composition and Widgets can consume predictable formats.
- Preserve current source compatibility where practical.
- Add probing and metadata APIs so callers can inspect images without full decode.
- Add encoding APIs for PNG, JPEG, and TIFF using the existing dependencies.
- Keep codec backends private and modular.
- Make error reporting specific enough for UI and asset pipeline diagnostics.

## Non-Goals

- Do not add WebP, AVIF, GIF, EXR, or SVG in the first completion pass.
- Do not expose `png_structp`, `tjhandle`, or `TIFF *` in public headers.
- Do not merge still-image codecs with audio/video processor APIs.
- Do not make URL loading the core abstraction. URL loading should remain a helper around byte-buffer loading.

## Proposed Public API

### 1. Split Format, Pixel Layout, And Alpha Into Stable Enums

Add new names and keep the existing enum values as deprecated aliases during migration.

```cpp
namespace OmegaWTK::Media {

    enum class ImageFormat : OPT_PARAM {
        Unknown,
        PNG,
        JPEG,
        TIFF
    };

    enum class ImagePixelFormat : OPT_PARAM {
        Unknown,
        Gray8,
        GrayAlpha8,
        RGB8,
        RGBA8,
        BGRA8
    };

    enum class ImageAlphaMode : OPT_PARAM {
        Unknown,
        Opaque,
        Straight,
        Premultiplied
    };

}
```

Rationale:

- `BitmapImage::Format` is nested under the decoded image type, but format describes encoded data.
- `BitmapImage::ColorFormat` does not distinguish channel depth or memory order.
- The renderer and UI image widget need a clear upload contract, especially for RGBA/BGRA and alpha handling.

### 2. Add RAII Pixel Storage

Replace raw public ownership with a move-only `BitmapImage` that owns its pixels. The first implementation can use `std::vector<ImgByte>` or a small custom storage wrapper. The public object should not expose an owning raw pointer as its primary storage.

```cpp
namespace OmegaWTK::Media {

    struct OMEGAWTK_EXPORT ImageSize {
        uint32_t width = 0;
        uint32_t height = 0;
    };

    struct OMEGAWTK_EXPORT ImageInfo {
        ImageFormat format = ImageFormat::Unknown;
        ImageSize size;
        ImagePixelFormat pixelFormat = ImagePixelFormat::Unknown;
        ImageAlphaMode alphaMode = ImageAlphaMode::Unknown;
        uint32_t channelCount = 0;
        uint32_t bitsPerChannel = 0;
        size_t stride = 0;
        bool hasColorProfile = false;
        bool isSRGB = false;
        bool hasGamma = false;
        double gamma = 0.0;
        uint32_t frameCount = 1;
    };

    struct OMEGAWTK_EXPORT ImageMetadata {
        ImgProfile profile;
        OmegaCommon::Vector<ImgByte> iccProfile;
        OmegaCommon::String mimeType;
        OmegaCommon::String comment;
        double dpiX = 0.0;
        double dpiY = 0.0;
    };

    struct OMEGAWTK_EXPORT BitmapImage {
        ImageInfo info;
        ImageMetadata metadata;
        OmegaCommon::Vector<ImgByte> pixels;

        ImgByte *data();
        const ImgByte *data() const;
        size_t byteSize() const;
        bool empty() const;
    };

}
```

Compatibility path:

- Keep the current fields temporarily, but populate them from `info` and `pixels`.
- Mark direct `data` ownership as deprecated once all internal call sites use `data()` and `byteSize()`.
- Fix the typoed old enum names by adding correct replacements and compatibility aliases where C++ allows it.

### 3. Add Decode Options

```cpp
namespace OmegaWTK::Media {

    enum class ImageOrientationPolicy : OPT_PARAM {
        IgnoreMetadata,
        ApplyMetadata
    };

    enum class ImageColorPolicy : OPT_PARAM {
        Preserve,
        ConvertToSRGB
    };

    struct OMEGAWTK_EXPORT ImageDecodeOptions {
        ImagePixelFormat preferredPixelFormat = ImagePixelFormat::RGBA8;
        ImageAlphaMode preferredAlphaMode = ImageAlphaMode::Straight;
        ImageOrientationPolicy orientationPolicy = ImageOrientationPolicy::ApplyMetadata;
        ImageColorPolicy colorPolicy = ImageColorPolicy::ConvertToSRGB;
        uint32_t maxWidth = 0;
        uint32_t maxHeight = 0;
        bool failOnUnsupportedMetadata = false;
    };

}
```

Rationale:

- PNG currently strips 16-bit data to 8-bit and expands palette/gray internally without exposing that policy.
- JPEG always decodes to RGBA but does not let the caller ask for RGB.
- TIFF may contain many layouts; `TIFFReadRGBAImage` normalizes but hides important source details.

The default should remain UI-friendly: 8-bit RGBA, straight alpha, top-left logical origin, sRGB-ready color.

### 4. Add Sniffing And Probe APIs

```cpp
namespace OmegaWTK::Media {

    OMEGAWTK_EXPORT ImageFormat detectImageFormat(OmegaCommon::StrRef extension);
    OMEGAWTK_EXPORT ImageFormat detectImageFormat(const ImgByte *data, size_t size);

    OMEGAWTK_EXPORT StatusWithObj<ImageInfo> probeImageFromFile(const OmegaCommon::FS::Path &path);
    OMEGAWTK_EXPORT StatusWithObj<ImageInfo> probeImageFromBuffer(const ImgByte *data, size_t size);
    OMEGAWTK_EXPORT StatusWithObj<ImageMetadata> readImageMetadataFromBuffer(const ImgByte *data, size_t size);

}
```

Rules:

- Byte sniffing wins over file extension.
- Extension detection remains as a fallback for empty or unseekable streams.
- Buffer APIs accept `const ImgByte *` because decoders should not mutate encoded input.

### 5. Add Finished Decode APIs

Keep current convenience names, but route them through a complete core decode API.

```cpp
namespace OmegaWTK::Media {

    OMEGAWTK_EXPORT StatusWithObj<BitmapImage> decodeImage(
        Core::IStream &stream,
        ImageFormat format = ImageFormat::Unknown,
        const ImageDecodeOptions &options = {});

    OMEGAWTK_EXPORT StatusWithObj<BitmapImage> loadImageFromFile(
        const OmegaCommon::FS::Path &path,
        const ImageDecodeOptions &options = {});

    OMEGAWTK_EXPORT StatusWithObj<BitmapImage> loadImageFromAssets(
        OmegaCommon::AssetBundle &bundle,
        const OmegaCommon::FS::Path &path,
        const ImageDecodeOptions &options = {});

    OMEGAWTK_EXPORT StatusWithObj<BitmapImage> loadImageFromBuffer(
        const ImgByte *bufferData,
        size_t bufferSize,
        ImageFormat format = ImageFormat::Unknown,
        const ImageDecodeOptions &options = {});

}
```

Compatibility path:

- Keep the old `BitmapImage::Format` overloads and forward them to `ImageFormat`.
- Keep `loadImageFromURL()` as a helper, but do not make codecs depend on networking.

### 6. Add Encode APIs

```cpp
namespace OmegaWTK::Media {

    struct OMEGAWTK_EXPORT ImageEncodeOptions {
        ImageFormat format = ImageFormat::PNG;
        uint32_t quality = 90;          // JPEG: 1-100. Ignored by PNG unless mapped later.
        uint32_t compressionLevel = 6;  // PNG/TIFF where supported.
        bool embedColorProfile = true;
        bool preserveMetadata = true;
    };

    OMEGAWTK_EXPORT Status saveImageToFile(
        const BitmapImage &image,
        const OmegaCommon::FS::Path &path,
        const ImageEncodeOptions &options = {});

    OMEGAWTK_EXPORT StatusWithObj<OmegaCommon::Vector<ImgByte>> encodeImageToBuffer(
        const BitmapImage &image,
        const ImageEncodeOptions &options = {});

}
```

Minimum required encoder behavior:

- PNG: encode `Gray8`, `RGB8`, and `RGBA8`.
- JPEG: encode `RGB8` and convert `RGBA8` by dropping alpha or compositing against an option-controlled matte color in a later phase.
- TIFF: encode `RGB8` and `RGBA8`.

### 7. Add Codec Capabilities

```cpp
namespace OmegaWTK::Media {

    struct OMEGAWTK_EXPORT ImageCodecCapabilities {
        ImageFormat format = ImageFormat::Unknown;
        bool canDecode = false;
        bool canEncode = false;
        bool canProbe = false;
        OmegaCommon::Vector<ImagePixelFormat> decodePixelFormats;
        OmegaCommon::Vector<ImagePixelFormat> encodePixelFormats;
    };

    OMEGAWTK_EXPORT OmegaCommon::Vector<ImageCodecCapabilities> availableImageCodecs();
    OMEGAWTK_EXPORT bool isImageFormatSupported(ImageFormat format);

}
```

This keeps capability checks centralized and lets asset tooling provide clear diagnostics.

## Private Codec Interface

Replace the current private `ImgCodec::readToStorage()` interface with explicit operations.

```cpp
namespace OmegaWTK::Media {

    class ImgCodec {
    public:
        virtual ~ImgCodec() = default;

        virtual ImageCodecCapabilities capabilities() const = 0;
        virtual StatusWithObj<ImageInfo> probe(Core::IStream &input) = 0;
        virtual StatusWithObj<ImageMetadata> readMetadata(Core::IStream &input) = 0;
        virtual StatusWithObj<BitmapImage> decode(Core::IStream &input,
                                                  const ImageDecodeOptions &options) = 0;
        virtual Status encode(const BitmapImage &image,
                              Core::OStream &output,
                              const ImageEncodeOptions &options) = 0;
    };

}
```

Implementation rule:

- Codec objects should be stateless or request-local. Do not store the input stream and output storage pointer as long-lived members.

Rationale:

- The current private API requires mutation of caller-provided storage and makes error handling indirect.
- Stateless decode/probe/encode calls are easier to test and safer for future threaded asset loading.

## Implementation Plan

### Phase 1: Stabilize Current Decoding — **Complete**

- [x] Add null checks in `loadImageFromBuffer()` and other codec creation paths. *(`loadFromBuffer` rejects null/empty buffers; `loadFromFile`/`loadFromBuffer` both null-check the codec before `readToStorage()`.)*
- [x] Fix JPEG to read the encoded stream into `dataBuf` before calling `tjDecompressHeader3()`. *(`JpegCodec.cpp:11-25` sizes the stream, allocates, and reads before header parsing.)*
- [x] Fill JPEG `ImageInfo`/legacy `header` fields: width, height, stride, channels, bit depth, RGBA pixel format, straight alpha or opaque alpha. *(All filled. `alpha_format` is `AlphaFormat::Ignore` — matches the PNG-RGB-without-tRNS precedent in the same module. The legacy `AlphaFormat` enum has no `Opaque` value; that semantic move lands with the Phase 2 `ImageAlphaMode` introduction.)*
- [x] Fill TIFF stride, alpha mode, compression, and bit depth consistently. *(`TiffCodec.cpp:102-112` populates all four.)*
- [x] Remove direct `std::cout` metadata logging from PNG and route metadata into `ImageMetadata`. *(All `std::cout`/`std::cerr` chunk dumps removed from `PngCodec.cpp`. The data that has a home in the legacy `BitmapImage` — gamma, sRGB, ICC profile — is still populated. Chunks with no destination today (bKGD/sCAL/cHRM/pCAL/sBIT/pHYs/tIME) are no longer logged and will be re-surfaced through `ImageMetadata` in Phase 2.)*
- [x] Add tests for valid PNG, JPEG, TIFF buffers and invalid/corrupt inputs. *(`wtk/tests/MediaCodecTest/MediaCodecTest.cpp` — 8 gtest cases covering 2x2 PNG/JPEG/TIFF buffers, null pointer, zero size, and a corrupt buffer per format. All 8 pass against the post-rebase `OmegaCommon::Img` API.)*
- [x] Remove the `loadImageFromAssets(path)` single-argument overload and the `IMPORT_IMG` macro that wraps it. *(Only `loadFromAssets(AssetBundle &, FS::Path)` exists; `IMPORT_IMG` and the `AssetLibrary::assets_res` fallback are gone from the codec layer.)*

#### Asset Bundle Loading Becomes Caller-Owned

`AppInst` currently auto-loads `./assets.pak` into an internal `Optional<AssetBundle>` during construction (`wtk/src/UI/App.cpp`). This auto-load was the only reason the no-bundle `loadImageFromAssets(path)` overload existed: it pulled the implicit bundle out of the global app instance.

Now that the overload is gone, the auto-load is an unfunded mandate. **`AppInst` should stop auto-loading `./assets.pak`.** Asset bundle ownership belongs with the application, not the framework. Applications that want a bundle should construct one explicitly (`OmegaCommon::AssetBundle::open(path)`) and pass it into the load APIs that need it.

This change is intentionally **not implemented in Phase 1** — it is a public-API behavior change that warrants its own pass with caller updates. Tracked here so the cleanup is not forgotten:

- Remove the `assetBundle` member, the `getAssetBundle()` accessors, and the auto-load block from `AppInst`.
- Audit existing apps under `wtk/tests/` and any sample apps for implicit dependence on the auto-loaded bundle. None should be assumed safe.
- The existing `loadImageFromAssets(bundle, path)` overload is the supported entry point for asset-backed image loading.

### Phase 2: Add RAII `BitmapImage` — **Complete (storage wrapper)**

The pixel buffer is owned by a new move-only `OmegaCommon::Img::PixelStorage`
wrapper (`common/include/omega-common/img.h`) with a type-erased deleter, so
each codec hands ownership over with whatever allocator the underlying
library requires. The proposal's final `OmegaCommon::Vector<Byte> pixels`
shape and the broader `ImageInfo`/`ImageMetadata` split are deferred to a
later phase — Phase 2 here is just the ownership fix, per the
"std::vector or a small custom storage wrapper" alternative in §2.

- [x] Introduce owned pixel storage and move semantics. *(`PixelStorage` is
      move-only with a custom-deleter `adopt()`, a `new[]`-based
      `allocate()`, and a non-owning `view()`. `BitmapImage` is move-only;
      copy is deleted, move is defaulted, default destructor releases via
      `PixelStorage::~PixelStorage`.)*
- [x] Update Composition/UI call sites to use `image.data()`. *(GTE
      `GEVulkanTextureAsset.cpp` now reads `img.data()` and drops the
      `BitmapImageOwner` wrapper that did `delete[] img.data` on every
      backend — UB for TIFF buffers. `BitmapTextureCache.cpp` uses
      `image->data()` / `image->empty()`. `MediaCodecTest` asserts use
      `img->data()` and the new `img->empty()` accessor. The proposed
      `image.info` move is still pending; `header` is the current source
      of truth.)*
- [x] Keep old fields populated during the transition. *N/A — the raw
      `Byte * data` field was removed outright in favor of the
      `pixels` storage member and the `data()` accessor. There were
      ~8 call sites and a single sweep was less risky than maintaining a
      shadow field that could drift out of sync with the owned buffer.*
- [x] Add a destructor or storage wrapper that handles all backend
      allocations consistently. *(`PixelStorage::reset()` runs the
      installed deleter; PNG/JPEG use `delete[]` via
      `PixelStorage::allocate`, TIFF uses `_TIFFfree` via
      `PixelStorage::adopt`.)*
- [x] Stop returning images with backend-specific allocations such as
      `_TIFFmalloc` unless the storage wrapper owns the matching deleter.
      *(`TiffCodec.cpp` adopts the `_TIFFmalloc` allocation with a
      `_TIFFfree` deleter; the buffer is freed correctly on
      `BitmapImage` destruction regardless of the consumer.)*

Typoed enum names `Pallete` and `Premultipled` remain in the public API;
those are slated for the Phase 2 enum-stabilization pass (§1) rather than
the RAII work tracked here.

#### Capture-path adapters (out-of-scope side note)

`OmegaWTK::Media::VideoFrame` reuses `BitmapImage` to carry decoded video
frames. The WMF (`WMFAudioVideoCapture.cpp`) and AVFoundation
(`AVFAudioVideoCapture.mm`) capture sinks used to write the platform
sample buffer pointer into the now-removed `BitmapImage::data` field —
relying on `BitmapImage` having no destructor. Both call sites now
install a non-owning `PixelStorage::view(...)` so the destructor leaves
the externally-owned sample buffer alone. The wider question — whether
video frames should keep piggy-backing on `BitmapImage` or move to a
dedicated `VideoFrameBuffer` type — is its own design decision and isn't
blocking Phase 2.

### Phase 3: Add Format Detection And Probe

**Not started.** Format selection is still extension-based (`imageFormatForExtension` in `ImgCodec.cpp`), buffer loads still require a caller-supplied `Format`, and there is no probe path.

- [ ] Implement magic-byte detection for PNG, JPEG, and TIFF.
- [ ] Make file/asset loading sniff bytes before falling back to extension.
- [ ] Add `probeImageFromFile()` and `probeImageFromBuffer()`.
- [ ] Add clear error messages for unsupported format, truncated input, and corrupt metadata.

### Phase 4: Add Decode Options

**Not started.** No `ImageDecodeOptions` type exists; PNG/JPEG/TIFF each pick their own normalization with no caller control.

- [ ] Implement `ImageDecodeOptions` for PNG, JPEG, and TIFF.
- [ ] Normalize the default decode path to `RGBA8`.
- [ ] Add RGB-only decode for JPEG and PNG when requested.
- [ ] Document top-left row order as the normalized output contract.
- [ ] Add size guard support through `maxWidth` and `maxHeight`.

### Phase 5: Add Encoding

**Not started.** The public API is still decode-only.

- [ ] Add `saveImageToFile()` and `encodeImageToBuffer()`.
- [ ] Implement PNG encoding first because it is the best lossless asset target.
- [ ] Implement JPEG encoding through TurboJPEG quality options.
- [ ] Implement TIFF encoding through libtiff for RGBA/RGB.
- [ ] Add round-trip tests: decode -> encode -> decode -> validate dimensions and pixel format.

### Phase 6: Add Capability Registry

**Not started.**

- [ ] Add `availableImageCodecs()` and `isImageFormatSupported()`.
- [ ] Keep the registry static because all codecs are compiled in by `AUTOMDEPS` and CMake today.
- [ ] If optional codecs are added later, convert the registry to build-flag-aware entries.

## Dependency Plan

No new `AUTOMDEPS` entries are required for the first completion pass.

| Feature | Dependency |
|---|---|
| PNG decode/probe/encode | `libpng`, `zlib` |
| JPEG decode/probe/encode | `libjpeg-turbo` |
| TIFF decode/probe/encode | `libtiff`, `zlib`, `libjpeg-turbo` |

Optional future formats should be separate proposals:

| Format | Candidate dependency | Reason to defer |
|---|---|---|
| WebP | `libwebp` | Useful for web assets, but not needed to finish current API. |
| AVIF | `libavif` plus AV1 codec backend | Adds complex transitive dependencies. |
| GIF | `giflib` | Animation requires a frame API, timing, and disposal semantics. |
| EXR | `OpenEXR` | HDR/float workflow needs renderer and texture-format decisions first. |

## Testing Plan

- Decode valid PNG, JPEG, and TIFF from file.
- Decode valid PNG, JPEG, and TIFF from memory buffer.
- Detect formats from magic bytes with wrong or missing file extensions.
- Probe dimensions without full pixel decode.
- Reject unsupported and corrupt input with specific errors.
- Verify row order is stable by decoding a small asymmetric fixture.
- Verify RGBA8 stride and byte size: `stride == width * 4`, `byteSize == stride * height`.
- Verify JPEG decode fills metadata enough for `Canvas::drawImage()` and `Image` widgets.
- Round-trip PNG, JPEG, and TIFF where encode support exists.

## Recommended First Patch

The first implementation patch should be intentionally small:

1. Fix JPEG stream reading and header population.
2. Add codec null checks and better errors in `ImgCodec.cpp`.
3. Add `ImageFormat` as a public alias-compatible enum and format sniffing helpers.
4. Add `probeImageFromBuffer()` for PNG/JPEG/TIFF.
5. Add tests for JPEG decode, invalid buffer decode, and format detection.

This patch delivers immediate correctness improvements without forcing the full RAII migration in the same change.

## Final API Shape

After migration, application code should look like this:

```cpp
using namespace OmegaWTK::Media;

ImageDecodeOptions options;
options.preferredPixelFormat = ImagePixelFormat::RGBA8;

auto imageResult = loadImageFromFile("assets/logo.png", options);
if (imageResult.isErr()) {
    // Show a clear asset loading error.
    return;
}

auto image = std::move(imageResult.value());
canvas->drawImage(imageHandle, rect);
```

Asset tools should look like this:

```cpp
auto info = probeImageFromFile("assets/photo.jpg");
if (info.isOk() && info.value().size.width > 4096) {
    // Warn about oversized assets before full decode.
}
```

Encoding should look like this:

```cpp
ImageEncodeOptions encodeOptions;
encodeOptions.format = ImageFormat::PNG;
encodeOptions.compressionLevel = 6;

saveImageToFile(bitmap, "out/snapshot.png", encodeOptions);
```

The key change is that `ImgCodec` becomes a complete still-image API: probe, decode, metadata, encode, capability reporting, and safe bitmap ownership.
