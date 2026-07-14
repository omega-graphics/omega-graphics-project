# PixelFormat Completion Plan

## Status — Rollout steps 1 + 2 LANDED (2026-07-14)

> **Done (2026-07-14):**
> - **Step 1 — enum + `pixelFormatInfo`.** The full grouped-range enum below is
>   live in `GTEBase.h` (color 0–63, depth 64–95, BC 96–127, ASTC 128–159, ETC
>   160–191), together with `PixelFormatInfo` / `pixelFormatInfo()` in
>   `GTEBase.cpp`. Compressed formats report `bytesPerTexel == 0` on purpose, so a
>   caller doing `w*h*bytesPerTexel` gets 0 rather than a plausible-but-wrong size.
>   The two per-backend `bytesPerTexel` switches (D3D12 `GED3D12Texture.cpp`,
>   Vulkan `GEVulkanCommandQueue.cpp`) are gone — each was a 2-case switch that
>   answered "4" for every format it did not know, i.e. the wrong row pitch for
>   anything wider than 8-bit RGBA.
> - **Step 2 — the three forward translation tables.** D3D12 / Metal / Vulkan all
>   extended. Per decision (a), unsupported families fail LOUDLY rather than
>   silently substituting: BC returns `MTLPixelFormatInvalid` on Metal (Apple GPUs
>   do not implement it — ship ASTC), and ASTC/ETC2 return `DXGI_FORMAT_UNKNOWN`
>   on D3D12. Vulkan can name every family.
> - **Migration:** confirmed no `PixelFormat`-to-int serialization exists, so the
>   renumbering (`RGBA8Unorm` 0 → 5) needed no mapping table. Asset loaders
>   translate through DXGI/MTL/VK formats, not the enum integer.
>
> **Also landed alongside (the reason this plan was pulled forward): depth
> attachments actually work now.** Depth had three independent defects, of which a
> missing format was only the first:
> 1. No depth `PixelFormat` existed. (Fixed here.)
> 2. `DepthStencilAttachment` had nowhere to name a depth texture, and Metal
>    "resolved" that by binding the COLOR render target as the depth AND stencil
>    attachment — a BGRA8 color texture is not a depth format, so this could never
>    work. Depth now lives on the render target
>    (`TextureRenderTargetDescriptor::depthTexture` →
>    `GETextureRenderTarget::depthTexture`), so a target owns the color surface and
>    the depth surface it is tested against and the two cannot drift apart.
>    `GENativeRenderTarget` deliberately has none: 3D renders into a texture target
>    with depth and is blitted to the drawable.
> 3. The Metal pipeline never set `depthAttachmentPixelFormat`, which leaves a
>    depth-enabled pipeline silently inert. Added
>    `RenderPipelineDescriptor::depthStencilPixelFormat` (+ the mesh equivalent)
>    and wired all three Metal pipeline paths.
> Plus: a depth texture may not carry `MTLTextureUsageShaderWrite` (Metal rejects
> it), now special-cased in `makeTexture`. And MRT color attachments now name a
> `GETextureRenderTarget` rather than a bare `GETexture`.
>
> **Verified** by `gte/tests/depth_format_test.cpp` (`omegagte_depth_format`) —
> 13/13 assertions against the real Metal device, including the negative case that
> a color-format texture is rejected as a depth surface.
>
> ### Architectural decision (2026-07-14): offscreen + blit is the standard
>
> `NativeRenderTargetDescriptor::allowDepthStencilTesting` is **DELETED** (2026-07-14).
> A native render target is color only; there is no longer any way to ask for depth
> on one. Enabling a render pass's `depthStencilAttachment` against a native target
> is a caller-contract violation and is reported by `startRenderPass`
> (`DEBUG_CRITICAL`, depth left unbound) on Metal and D3D12.
>
> The reasoning: **no graphics API hands you a swapchain WITH depth.** D3D12,
> Vulkan and Metal all give you color images only; a depth buffer is always a
> separate texture you allocate. Supporting the flag would mean allocating a
> companion depth surface per back buffer — which neither backend ever did. D3D12
> built a DSV heap whose descriptors aliased the COLOR back buffer (invalid) and
> Metal ignored the flag and bound the color drawable as the depth attachment (also
> invalid). Both backends independently produced the same silently-broken depth, so
> the flag read as supported and lied.
>
> **The standard is: render 3D OFFSCREEN into a `GETextureRenderTarget` that owns
> its depth surface, then blit / resolve to the drawable.** This is what Unreal
> (SceneColor + SceneDepthZ → final pass to backbuffer) and Unity's SRP
> (`_CameraColorAttachment` + `_CameraDepthAttachment` → final blit) both do by
> default. The forcing function is not depth but FORMAT: a drawable is
> `BGRA8Unorm`, so HDR, tonemapping, TAA, deferred G-buffers and compute readback
> are all impossible against it. Once the scene must live in an offscreen float
> target anyway, the blit is free and direct-to-drawable buys nothing.
>
> **Still open:** `supportsFormat()` on `OmegaGraphicsEngine` (rollout step 2's
> device-capability query), rollout steps 3–5 (Vulkan aspect-mask wiring, asset
> loader reverse mappings, compressed-asset upload path).

## Original status (pre-2026-07-14)

`OmegaGTE::PixelFormat` (defined in
[gte/include/omegaGTE/GTEBase.h:976](../include/omegaGTE/GTEBase.h)) ships
five entries today: `RGBA8Unorm`, `RGBA16Unorm`, `RGBA8Unorm_SRGB`,
`BGRA8Unorm`, `BGRA8Unorm_SRGB`. That's enough to bring up a swapchain
and one HDR-adjacent path; it is not enough to support depth/stencil
attachments, compressed asset uploads, single-channel glyph atlases,
HDR framebuffers, or shadow-map sampling. Each of those is on the
roadmap in an adjacent plan.

This plan defines the target enum, the naming convention, the
per-backend translation work, and the helper API that should ride
along.

## Driving requirements

| Need | Source |
|------|--------|
| Depth / stencil formats so `RenderTargetAndDepthStencil` differs from `RenderTarget` | [Vulkan-Texture-Memory-Plan.md §301–308](Vulkan-Texture-Memory-Plan.md) |
| `getOrCreateSwizzledView` aspect-mask derivation | [Vulkan-Texture-Memory-Plan.md §310–313](Vulkan-Texture-Memory-Plan.md) |
| Compressed-format upload path (BC / ASTC / ETC) | [Vulkan-Texture-Memory-Plan.md §36, §272](Vulkan-Texture-Memory-Plan.md) |
| Multi-format MRT via `colorPixelFormats` vector | [Pipeline-Completion-Extension-Plan.md §367–378](Pipeline-Completion-Extension-Plan.md) |
| `BlitPipeline` format-conversion (`RGBA16Float → RGBA8Unorm` tone-map) | [Pipeline-Completion-Extension-Plan.md §538–563](Pipeline-Completion-Extension-Plan.md) |
| Depth textures + comparison samplers (`depth2d` / `depth2d_array` / `depthcube`) | [OmegaSL-Feature-Gap-Survey.md §2.2](OmegaSL-Feature-Gap-Survey.md) |
| Single-channel formats for glyph rendering | [Vulkan-Texture-Memory-Plan.md §47–49](Vulkan-Texture-Memory-Plan.md) |

## Naming convention

`<channels><bits><type>[_SRGB]`, matching the five entries already
shipped. Examples: `RG16Float`, `BC7_RGBA_Unorm_SRGB`,
`D32Float_S8Uint`. Compressed families get the block size before the
channel layout (`ASTC_4x4_Unorm`), keeping them visually distinct
from uncompressed entries.

The convention is the one decision in this plan that is structurally
expensive to change later — every translation table and every doc
inherits it. If you want to switch to a different convention (e.g.
DXGI's `R8G8B8A8_UNORM` shape), do it now.

## Decisions

1. **Compressed coverage — baseline subset.** Full BC1–BC7 + every
   ASTC block + ETC2 R/RG variants is ~30 entries and a translation
   nightmare on Metal (no BC on Apple Silicon). Baseline: BC1 / BC3 /
   BC5 / BC7 (covers diffuse / diffuse-with-alpha / normal / quality),
   ASTC 4×4 / 6×6 / 8×8 (low/mid/high), ETC2 RGB / RGBA / R11 (Android
   defaults). Easy to extend later when a real caller asks.

2. **Depth/stencil packing — combined entries.** `D24Unorm_S8Uint`
   and `D32Float_S8Uint` as single enum entries, matching D3D12 /
   Vulkan / Metal's combined formats. No standalone `S8Uint` — Metal
   lacks it on macOS GPUs and the demand is zero.

3. **Integer textures — minimum viable.** `R32Uint` (compute
   readback, indirect args), `R16Uint` (hi-Z). Skip the
   RG/RGBA/Snorm/Sint integer family until a real caller appears.
   Vertex attributes already have their own
   [`VertexFormat` enum](Pipeline-Completion-Extension-Plan.md) and
   don't pull on `PixelFormat`.

4. **Packed/HDR-lite formats — both.** `R11G11B10Float` for cheap HDR
   framebuffers, `RGB10A2Unorm` for 10-bit displays / packed normals.
   Wide cross-backend support; both pull their weight.

5. **sRGB twins for every compressed color format.** BC1 / BC2 / BC3
   / BC7 and most ASTC formats have sRGB siblings. Ship the twin —
   the alternative is forcing every caller into a manual gamma decode
   in their fragment shader.

6. **Value stability — grouped numeric ranges.** Color = 0–63,
   depth/stencil = 64–95, BC = 96–127, ASTC = 128–159, ETC = 160–191.
   Adding a new format slots into its group without renumbering
   neighbours, which would otherwise be a soft-ABI break for anything
   that has serialized a `PixelFormat` to disk. Costs gaps in the
   numeric space, zero runtime overhead.

## Target enum

```cpp
/// @brief Describes a pixel format for render targets, textures, and pipelines.
/// @paragraph Values are grouped into numeric ranges (color = 0–63,
/// depth/stencil = 64–95, BC = 96–127, ASTC = 128–159, ETC = 160–191) so new
/// entries can be appended within their group without renumbering siblings.
enum class PixelFormat : int {
    // ── 8-bit color ──────────────────────────────────────────────
    R8Unorm                     = 0,    // glyph atlas, single-channel masks
    R8Snorm                     = 1,
    R8Uint                      = 2,
    RG8Unorm                    = 3,    // 2-channel normals, packed data
    RG8Snorm                    = 4,
    RGBA8Unorm                  = 5,    // existing
    RGBA8Unorm_SRGB             = 6,    // existing
    RGBA8Snorm                  = 7,
    BGRA8Unorm                  = 8,    // existing — Windows swapchain default
    BGRA8Unorm_SRGB             = 9,    // existing

    // ── 16-bit color ─────────────────────────────────────────────
    R16Unorm                    = 16,
    R16Float                    = 17,   // single-channel HDR / hi-Z
    R16Uint                     = 18,
    RG16Unorm                   = 19,
    RG16Float                   = 20,
    RGBA16Unorm                 = 21,   // existing
    RGBA16Float                 = 22,   // HDR framebuffer, BlitPipeline tone-map src

    // ── 32-bit color ─────────────────────────────────────────────
    R32Float                    = 32,   // depth approximations, compute output
    R32Uint                     = 33,   // compute readback, indirect args
    RG32Float                   = 34,
    RGBA32Float                 = 35,   // ground-truth HDR

    // ── Packed ───────────────────────────────────────────────────
    RGB10A2Unorm                = 48,   // 10-bit displays, packed normals
    R11G11B10Float              = 49,   // HDR-lite framebuffer

    // ── Depth / stencil (Vulkan plan §301 gap) ───────────────────
    D16Unorm                    = 64,
    D32Float                    = 65,
    D24Unorm_S8Uint             = 66,
    D32Float_S8Uint             = 67,

    // ── Block-compressed: BC (desktop) ───────────────────────────
    BC1_RGBA_Unorm              = 96,   // diffuse, 1-bit alpha
    BC1_RGBA_Unorm_SRGB         = 97,
    BC3_RGBA_Unorm              = 98,   // diffuse with full alpha
    BC3_RGBA_Unorm_SRGB         = 99,
    BC5_RG_Unorm                = 100,  // normal maps (2-channel)
    BC7_RGBA_Unorm              = 101,  // high-quality color
    BC7_RGBA_Unorm_SRGB         = 102,

    // ── Block-compressed: ASTC (mobile / cross-platform) ─────────
    ASTC_4x4_Unorm              = 128,
    ASTC_4x4_Unorm_SRGB         = 129,
    ASTC_6x6_Unorm              = 130,
    ASTC_6x6_Unorm_SRGB         = 131,
    ASTC_8x8_Unorm              = 132,
    ASTC_8x8_Unorm_SRGB         = 133,

    // ── Block-compressed: ETC2 (Android baseline) ────────────────
    ETC2_RGB8_Unorm             = 160,
    ETC2_RGB8_Unorm_SRGB        = 161,
    ETC2_RGBA8_Unorm            = 162,
    ETC2_RGBA8_Unorm_SRGB       = 163,
    EAC_R11_Unorm               = 164,  // single-channel compressed
};
```

## Helper API — `pixelFormatInfo`

The enum is only half the work. Every backend currently re-derives
`bytesPerTexel` from a `switch` (e.g.
[gte/src/d3d12/GED3D12Texture.cpp:189](../src/d3d12/GED3D12Texture.cpp),
[gte/src/vulkan/GEVulkanCommandQueue.cpp:1986](../src/vulkan/GEVulkanCommandQueue.cpp)).
With 40 entries those switches become unmaintainable. Add one source
of truth:

```cpp
struct PixelFormatInfo {
    enum class Aspect : std::uint8_t {
        Color,
        Depth,
        Stencil,
        DepthStencil,
    };

    Aspect        aspect          = Aspect::Color;
    std::uint8_t  bytesPerTexel   = 4;   // 0 if compressed — use blockBytes
    std::uint8_t  blockWidth      = 1;   // 1 for uncompressed
    std::uint8_t  blockHeight     = 1;
    std::uint8_t  blockBytes      = 0;   // 0 if uncompressed — use bytesPerTexel
    bool          isCompressed    = false;
    bool          isSRGB          = false;
    std::uint8_t  channelCount    = 4;
};

OMEGAGTE_EXPORT PixelFormatInfo pixelFormatInfo(PixelFormat);
```

`pixelFormatInfo` is a free function in `OmegaGTE` namespace — its
data is device-independent (just bit-counts and channel structure).
Device-dependent queries ("is this format renderable on *this*
adapter?") belong on `OmegaGraphicsEngine`:

```cpp
class OmegaGraphicsEngine {
    ...
    enum class FormatUsage : std::uint32_t {
        SampledImage        = 1u << 0,
        StorageImage        = 1u << 1,
        ColorAttachment     = 1u << 2,
        DepthStencilAttachment = 1u << 3,
        Blit                = 1u << 4,
    };
    virtual bool supportsFormat(PixelFormat, FormatUsage) = 0;
};
```

D3D12 implements via `CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT)`,
Vulkan via `vkGetPhysicalDeviceFormatProperties` (generalizing the
existing format-guard from the Vulkan plan), Metal via the documented
per-format capability tables.

## Per-backend translation work

Three tables to extend, all currently single-`switch` functions:

| Function | File | Today | Adds |
|----------|------|-------|------|
| `pixelFormatToDxgiFormat` | [gte/src/d3d12/GED3D12.cpp:23](../src/d3d12/GED3D12.cpp) | 5 cases | ~35 new cases |
| `pixelFormatToMTLPixelFormat` | [gte/src/metal/GEMetal.mm:27](../src/metal/GEMetal.mm) | 5 cases | ~35 new cases, BC family errors on Apple Silicon |
| `pixelFormatToVkFormat` | [gte/src/vulkan/GEVulkan.cpp:35](../src/vulkan/GEVulkan.cpp) | 5 cases | ~35 new cases |

And two reverse mappings on the asset-load path:

| Function | File | Notes |
|----------|------|-------|
| `mapDxgiPixelFormat` | [gte/src/d3d12/GED3D12TextureAsset.cpp:40](../src/d3d12/GED3D12TextureAsset.cpp) | DirectXTex round-trip |
| `mapMetalPixelFormat` | [gte/src/metal/GEMetalTextureAsset.mm:19](../src/metal/GEMetalTextureAsset.mm) | MTKTextureLoader round-trip |

### Metal compressed-format question

Apple Silicon GPUs don't support BC formats. Two options:

- **(a) Error at `makeTexture` / `makeRenderPipelineState`.** Caller
  is responsible for picking ASTC instead. `supportsFormat()` lets
  them probe first. Cheapest to implement, pushes the policy to the
  caller.
- **(b) Auto-transcode BC → ASTC at upload.** Hides the
  cross-platform asymmetry; costs implementation work for every BC
  variant and a noticeable upload-time penalty.

**Recommendation: (a) for v1.** Transcoding is a separate body of
work and most asset pipelines already ship ASTC alongside BC for
exactly this reason.

## Migration path

The existing five entries keep their semantics. `RGBA8Unorm = 5`
under grouped ranges is *not* the same integer as today (where it
sits at index 0), so anything that has serialized a `PixelFormat`
to disk needs a migration — but: a quick grep shows no
`PixelFormat`-to-int serialization in the engine today, and the
asset-load paths translate through DXGI/MTL/VK formats rather than
the enum integer. Confirm before landing; if a serialized usage
turns up, write a one-time mapping table.

`TexturePixelFormat` is just a `using` alias for `PixelFormat` at
[gte/include/omegaGTE/GETexture.h:11](../include/omegaGTE/GETexture.h),
so it inherits all changes automatically.

## Rollout

1. **Land the enum + `pixelFormatInfo` together.** No backend wiring
   yet — just the header change and the helper. Existing code keeps
   compiling because the five existing entries are still present
   (under new integer values, but symbolically identical). Replace
   the per-backend `bytesPerTexel` switches with
   `pixelFormatInfo(fmt).bytesPerTexel`. One PR; mechanical.

2. **Extend the three forward translation tables.** Every new entry
   gets its D3D12/Metal/Vulkan equivalent or an explicit
   "unsupported on this backend" path. Add `supportsFormat()` to
   `OmegaGraphicsEngine` with backend overrides. One PR per backend,
   parallelizable.

3. **Wire depth/stencil aspect into the existing TODO sites.**
   - [Vulkan plan §301](Vulkan-Texture-Memory-Plan.md) —
     `RenderTargetAndDepthStencil` arm branches on
     `pixelFormatInfo(fmt).aspect`.
   - [Vulkan plan §310](Vulkan-Texture-Memory-Plan.md) —
     `getOrCreateSwizzledView` derives `aspectMask` from the same
     helper.
   - D3D12 `isDSV` already exists; just route it through
     `pixelFormatInfo`.
   - Metal is already format-driven; verify combined-format mapping
     produces the right `MTLPixelFormatDepth32Float_Stencil8` family.

4. **Extend asset-loader reverse mappings.** DirectXTex and
   MTKTextureLoader can both surface formats the engine doesn't
   support today; once the enum is wider, fewer of those need the
   `RGBA8Unorm` fallback in their default arms.

5. **Compressed-asset upload path.** Vulkan plan §272 calls this out
   as deferred; it can land now that BC/ASTC/ETC have enum entries.
   Sibling to step 4.

## Out of scope for v1

- **YCbCr / planar formats.** Video / camera input territory; needs
  `VkSamplerYcbcrConversion` on Vulkan and is a poor fit for the
  first pass of a graphics-engine format enum.
- **Sparse-texture-specific formats.** Tile-shape constraints
  diverge by GPU; defer to a sparse-textures work item.
- **HDR ASTC / 3D-texture-only formats.** Defer until an HDR or
  volumetric asset pipeline asks.
- **The full BC family (BC2, BC4, BC6H_SF16 / BC6H_UF16, BC7 sRGB
  twin if missed).** Easy to add into the reserved range when a real
  caller appears.

## Open questions

- **Should `PixelFormat` move into its own header** (e.g.
  `gte/include/omegaGTE/PixelFormat.h`) instead of living in
  `GTEBase.h`? Adding 35 entries plus `PixelFormatInfo` triples the
  format-related code in `GTEBase.h`. Defer until the file feels
  cluttered.
- **Naming convention sanity check.** This plan commits to
  `<channels><bits><type>` (current). If you want to switch to
  DXGI-shape (`R8G8B8A8_UNORM`), Vulkan-shape (`R8G8B8A8_UNORM` with
  underscore separators), or anything else, change it now — every
  translation table and every doc inherits the choice.
