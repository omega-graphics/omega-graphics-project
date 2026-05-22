# KTX Texture Codec — Implementation Plan

## Goal

Add first-class **KTX / KTX2** texture loading to OmegaGTE as a **dedicated GTE
codec** (not an `OmegaCommon::Img` bitmap codec), preserving the format's full
GPU fidelity: block-compressed payloads (BC / ASTC / ETC2), HDR float formats,
complete **mip chains**, **cube faces**, and **array layers**. A loaded `.ktx` /
`.ktx2` file uploads to a `GETexture` with the same shape it had on disk — no
forced decode to RGBA8.

The codec is backed by **libktx** (KhronosGroup/KTX-Software), vendored through
`gte/AUTOMDEPS` and built from source like the other GTE third-party libs.

## Why this lives in GTE, not OmegaCommon::Img

`OmegaCommon::Img` (`ImgCodec` / `BitmapImage`) is, by design, a **decoded
CPU-side linear bitmap** layer: PNG / JPEG / TIFF normalized to RGB/RGBA8. KTX is
a **GPU-texture container** — it carries GPU concepts that `BitmapImage` cannot
model (compressed blocks, mip chains, cube/array/3D shape, GPU format enums).
GTE already owns those concepts (`GETexture`, `TextureDescriptor`, `PixelFormat`,
`TextureKind`, `mipLevels`, `arrayLayers`). So the KTX codec belongs at the GPU
layer. This supersedes the open question in
`GEMesh-TextureAssets-Implementation-Plan.md` §3.4 ("should KTX loading be a part
of OmegaCommon::Img or TextureAsset?") and the "fold stb_image in for KTX" note
(stb_image does not handle KTX regardless).

## Current State

- **`PixelFormat`** (`gte/include/omegaGTE/GTEBase.h`) models only **5
  uncompressed formats**: `RGBA8Unorm`, `RGBA16Unorm`, `RGBA8Unorm_SRGB`,
  `BGRA8Unorm`, `BGRA8Unorm_SRGB`. No BC / ASTC / ETC / HDR-float. A KTX format
  enum has nowhere to map today.
- **`GETexture` upload** is mip-0 only: `copyBytes(bytes, bytesPerRow)` and the
  region overload both target mip 0 with a **row-based** layout. There is **no
  API to upload a specific mip level / array layer / cube face**, and the
  row-stride model does not describe block-compressed data.
- **Vulkan** `copyBytes` lands on a `LINEAR`-tiled `HOST_VISIBLE` image and does
  a direct `memcpy` — it cannot host compressed, mipped, or cube/array textures
  (those need `OPTIMAL` tiling + a staging buffer + `vkCmdCopyBufferToImage`).
- **`GETextureAsset`** picks a backend impl at build time
  (`GEMetalTextureAsset.mm`, `GEVulkanTextureAsset.cpp`,
  `GED3D12TextureAsset.cpp`). Metal uses `MTKTextureLoader` (already KTX-capable);
  D3D12 uses DirectXTex; Vulkan decodes through `OmegaCommon::Img` (RGBA8 only).
- **`gte/src/common/`** already holds backend-neutral helpers (`MeshParser`,
  `GEMesh`, shared `GETextureAsset` base) — the natural home for a shared codec.
- **Build/verify reality:** only the **Metal** backend compiles on the current
  macOS host; D3D12 / Vulkan are write-only here and need CI to build (see the
  project's backend-build-verification note). Plan accordingly: Metal is the only
  locally-verifiable upload path.

## Non-Goals

- Replacing `OmegaCommon::Img` or routing PNG/JPEG/TIFF through libktx.
- A from-scratch KTX parser — libktx is the dependency of record.
- Encoding / writing KTX files (read/upload only).
- Runtime mip **generation** for KTX (KTX ships its own mips; generation stays
  the existing best-effort hint for the bitmap path).
- Modeling *every* GPU pixel format. v1 adds a curated set (owned by Extension 7
  §7.4); the enum can grow later without reworking the codec.

---

## Prerequisites: Extension 7 (GETexture API Completion)

This plan is **layered on `Pipeline-Completion-Extension-Plan.md` Extension 7**,
which owns the generic `GETexture` capabilities a KTX loader needs. Those
features are not redefined here — they land first, then this plan consumes them:

- **§7.4 Compressed pixel formats** — extends `PixelFormat` (`GTEBase.h`) with the
  BC / ASTC / ETC2 families (+ a `getBlockSize()` traits accessor) and the
  per-backend `DXGI_FORMAT` / `MTLPixelFormat` / `VkFormat` maps. The KTX format
  enum maps onto these; the **v1 format-set decision lives in §7.4**, not here.
- **§7.1 `TextureRegion` mip/layer + §7.8 full subresource upload** — the mechanism
  to upload an arbitrary `{mipLevel, arrayLayer, face}` subresource, including the
  cube-face → slice flatten helper, the `bytesPerImage` slice-pitch overload, and
  the **Vulkan `OPTIMAL`-tiling + staging-buffer path** (the heaviest engine
  change — it is §7.8's work, not this plan's).
- **§7.3 metadata accessors + §7.9 per-mip dimensions** — `getWidth/Height/Depth/
  MipLevels/PixelFormat` and `getMipDimensions(level)`, used by the codec's upload
  loops to size each region without backend introspection.
- **§7.10 initial-data-at-create (optional)** — if present, the loader can hand the
  whole packed pyramid to `makeTexture` in one call and let the backend pick
  optimal tiling, instead of N `copyBytes` calls.

If Extension 7 has not landed, those subsections are the blocking pre-work for
this plan. Everything below assumes they exist.

---

## Phase 1: Dependency — vendor libktx

### 1.1 Vendor libktx via `gte/AUTOMDEPS`

- Add a `git` dependency `libktx` → `https://github.com/KhronosGroup/KTX-Software.git`,
  `dest: $(external_lib_path)/KTX-Software`, pinned to a release tag, for
  `windows`, `linux`, `android`, **and** `macos` (the codec is shared across all
  backends — see Phase 3).
- CMake (`gte/CMakeLists.txt`): `add_subdirectory` (or `ExternalProject`,
  matching how DirectXTex / VMA are wired) with libktx configured for a **static**
  build and no extra surface:
  - `KTX_FEATURE_STATIC_LIBRARY=ON`
  - `KTX_FEATURE_TESTS=OFF`, `KTX_FEATURE_TOOLS=OFF`, `KTX_FEATURE_DOC=OFF`
  - `KTX_FEATURE_GL_UPLOAD=OFF`, `KTX_FEATURE_VK_UPLOAD=OFF` — we do our own GPU
    upload through the engine, so libktx's GL/VK uploaders are dead weight.
  - Keep Basis transcode enabled (it bundles basisu + zstd; that is the point).
- Link `ktx` PRIVATE into the OmegaGTE target. libktx types must **not** leak
  into public GTE headers (same discipline as libpng in OmegaCommon).
- **Files:** `gte/AUTOMDEPS`, `gte/CMakeLists.txt`.

> **Pixel-format model + subresource upload:** provided by Extension 7 §7.4
> (compressed `PixelFormat` families + traits) and §7.1 / §7.8 (subresource
> upload + Vulkan staging path). See **Prerequisites** above. This plan does not
> redefine them.

---

## Phase 2: The KTX codec (`gte/src/common/KtxCodec`)

Backend-neutral. Wraps libktx; produces an engine-shaped descriptor + raw
subresource access. No GPU calls here — pure parse/transcode.

### 2.1 Parse + classify

- `KtxCodec::loadFromFile(path)` / `loadFromMemory(bytes,size)` →
  `ktxTexture_CreateFromNamedFile` / `ktxTexture2_CreateFromMemory`.
- Produce a `KtxImage` value type:
  - engine `PixelFormat`, `width/height/depth`, `mipLevels`, `arrayLayers`,
    `faceCount`, derived `TextureKind` (faces==6 → `TexCube`/`TexCubeArray`;
    depth>1 → `Tex3D`; layers>1 → array; else `Tex2D`), `isSRGB`.
  - per-`(level, layer, face)` accessor returning `{ptr, byteCount, bytesPerRow}`
    via `ktxTexture_GetImageOffset` + `ktxTexture_GetImageSize` over
    `ktxTexture_GetData`.
- Map source format → engine `PixelFormat`: KTX2 carries `vkFormat`; KTX1 carries
  `glInternalformat`. Unsupported formats → clear `Result` error naming the
  format (no silent RGBA8 fallback).

### 2.2 KTX2 Basis / UASTC transcoding

- If `ktxTexture2_NeedsTranscoding`, call `ktxTexture2_TranscodeBasis` to a target
  chosen by what the engine/platform supports — preference order:
  desktop → `KTX_TTF_BC7_RGBA`; mobile → `KTX_TTF_ASTC_4x4_RGBA`; universal
  fallback → `KTX_TTF_RGBA32`. (Decision point: exact target-selection policy.)
- After transcode, re-map to the engine `PixelFormat` and continue as in 3.1.
- **Files:** new `gte/src/common/KtxCodec.h`, `gte/src/common/KtxCodec.cpp`.
  Compiled on all backends (unlike `MeshParser`, which Metal drops — KTX via
  libktx is the canonical shared path for every backend).

---

## Phase 3: Wire KtxCodec into GETextureAsset

- In each backend's `GETextureAsset::load`, detect `.ktx` / `.ktx2` by extension
  and route to `KtxCodec` instead of the bitmap path:
  1. `KtxCodec` → `KtxImage`.
  2. Build `TextureDescriptor` (format, w/h/d, mipLevels, arrayLayers, kind,
     sRGB) and `engine->makeTexture`. If §7.10 landed, pass the packed pyramid as
     `initialData` + `initialDataLayout` and skip step 3.
  3. For each `(level, layer, face)`, upload via the §7.8 subresource overload
     (`copyBytes(bytes, bytesPerRow, bytesPerImage, region)` with
     `region.mipLevel/arrayLayer/face` set; size each region from
     `getMipDimensions(level)` §7.9).
  4. `descriptor()` reports the true format / mips / kind (now backend-neutral
     via §7.3 accessors).
- **Vulkan:** non-RGBA8 / mipped / cube KTX go through the §7.8 OPTIMAL+staging
  path; the existing PNG/JPEG/TIFF RGBA8 path is untouched.
- **Metal:** route KTX through the shared `KtxCodec` for consistent fidelity
  across backends. (Alternative: keep `MTKTextureLoader` for `.ktx` — simpler but
  diverges per-platform. Recommend the shared path; flag as a decision point.)
- **D3D12:** DirectXTex stays for DDS/WIC; `.ktx`/`.ktx2` routes through
  `KtxCodec`.
- **Files:** `GEMetalTextureAsset.mm`, `GEVulkanTextureAsset.cpp`,
  `GED3D12TextureAsset.cpp`.

---

## Phase 4: Tests + documentation

- Test assets + load/upload tests per backend (CI for Vulkan/D3D12): KTX1
  uncompressed RGBA8; KTX2 Basis (transcoded); a BC7 and an ASTC texture; a
  cubemap; a mipped texture. Verify `descriptor()` shape and (where possible)
  readback or visual.
- Update `GETextureAsset.h` doc comment (currently says Vulkan uses
  "libktx + stb_image" — correct it to the real split).
- Add a `gte/docs` guide: how KTX flows through `KtxCodec` → `GETexture`, and the
  supported format set (the `PixelFormat` enum itself is documented with §7.4).

---

## Implementation order (suggested)

0. **Extension 7 pre-work** (§7.4 compressed formats, §7.1 + §7.8 subresource
   upload, §7.3 + §7.9 accessors). Blocking; tracked in
   `Pipeline-Completion-Extension-Plan.md`.
1. **1.1** libktx vendored + building static, linked into OmegaGTE.
2. **2.1–2.2** `KtxCodec` (parse, classify, Basis transcode).
3. **3** Wire into `GETextureAsset` per backend (Metal first — only locally
   verifiable; Vulkan/D3D12 to CI).
4. **4** Tests + docs.

## Open decisions (confirm before/while implementing)

- **v1 format set** — owned by Extension 7 §7.4 (which families ship first). KTX
  support is bounded by whatever §7.4 lands.
- **Basis transcode target policy** (§2.2) — platform-adaptive
  (BC7/ASTC) vs. always-RGBA32 for v1 simplicity.
- **Metal KTX path** (Phase 3) — unify on shared `KtxCodec`, or keep
  `MTKTextureLoader` for `.ktx` on Metal only.
- **Cube/array/3D scope** — descriptor + codec express all shapes; confirm
  Extension 7 §7.8's upload path covers every `TextureKind` in v1, or start with
  2D + mips + cube.

## Summary

| Area | Change |
|------|--------|
| **Prerequisite** | Extension 7 §7.4 (compressed `PixelFormat`), §7.1/§7.8 (subresource upload + Vulkan staging), §7.3/§7.9 (accessors). Not redefined here. |
| **Dependency** | libktx vendored via `gte/AUTOMDEPS`, static, GL/VK uploaders disabled, Basis transcode kept. |
| **KtxCodec** | New backend-neutral `gte/src/common/KtxCodec` wrapping libktx → engine-shaped `KtxImage` + subresource access; KTX2 Basis transcode. |
| **GETextureAsset** | `.ktx`/`.ktx2` routed through `KtxCodec` on all backends, uploading full mip/layer/face data via the §7.8 overload. |
