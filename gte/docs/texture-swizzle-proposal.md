# Feature Proposal: Texture Swizzle Support

## Motivation

OmegaGTE currently has no way to remap texture channels at read/sample time. When a texture stores data as RGBA but the shader needs it as BGRA (or needs a single channel broadcast like `R → RRRR`), the only options today are:

1. Convert on the CPU before upload.
2. Write a dedicated shader pass to rewrite the texture into a new allocation.
3. Hardcode the remapping inside the shader itself.

All three of these are wasteful — every backend API already provides zero-cost channel remapping at the view/descriptor level:

- **Vulkan:** `VkComponentMapping` in `VkImageViewCreateInfo`
- **Metal:** `newTextureViewWithPixelFormat:textureType:levels:slices:swizzle:` (`MTLTextureSwizzleChannels`)
- **D3D12:** `D3D12_SHADER_COMPONENT_MAPPING` in SRV descriptors

This proposal adds a cross-platform texture swizzle abstraction to OmegaGTE, exposing this hardware-native capability through the public API and the OmegaSL shader language.

---

## Scope

| In scope | Out of scope |
|----------|-------------|
| Channel swizzle on texture sampling/read | Format reinterpretation (e.g. viewing R32Float as R32Uint) |
| Public `TextureSwizzle` descriptor type | Full `TextureView` object with mip/slice subranges |
| Swizzle at texture bind time (runtime API) | Swizzle literals inside OmegaSL shader source |
| Backend mapping for Vulkan, Metal, D3D12 | Swizzle on render target / storage image writes |

A full `TextureView` type (subresource ranges, format reinterpretation) is a natural follow-up but is deliberately excluded here to keep this change minimal and self-contained.

---

## Design

### 1. Public API types

Add to `GTEBase.h`:

```cpp
/// Individual channel source for a swizzle mapping.
enum class TextureSwizzleChannel : unsigned char {
    Red,      ///< Source the red channel
    Green,    ///< Source the green channel
    Blue,     ///< Source the blue channel
    Alpha,    ///< Source the alpha channel
    Zero,     ///< Constant 0
    One,      ///< Constant 1
    Identity  ///< Passthrough (use the channel's own position)
};

/// Describes how texture channels are routed when sampled or read.
struct TextureSwizzle {
    TextureSwizzleChannel r = TextureSwizzleChannel::Identity;
    TextureSwizzleChannel g = TextureSwizzleChannel::Identity;
    TextureSwizzleChannel b = TextureSwizzleChannel::Identity;
    TextureSwizzleChannel a = TextureSwizzleChannel::Identity;

    /// Convenience: identity swizzle (no remapping).
    static TextureSwizzle identity() { return {}; }

    /// Convenience: broadcast red into all four channels.
    static TextureSwizzle broadcastRed() {
        return { TextureSwizzleChannel::Red,
                 TextureSwizzleChannel::Red,
                 TextureSwizzleChannel::Red,
                 TextureSwizzleChannel::Red };
    }

    /// Convenience: swap R and B channels (RGBA ↔ BGRA).
    static TextureSwizzle swapRB() {
        return { TextureSwizzleChannel::Blue,
                 TextureSwizzleChannel::Green,
                 TextureSwizzleChannel::Red,
                 TextureSwizzleChannel::Alpha };
    }

    bool isIdentity() const {
        return r == TextureSwizzleChannel::Identity
            && g == TextureSwizzleChannel::Identity
            && b == TextureSwizzleChannel::Identity
            && a == TextureSwizzleChannel::Identity;
    }
};
```

### 2. Integration point: texture binding

The swizzle applies at **bind time**, not at texture creation. This keeps `GETexture` as a plain data allocation and avoids multiplying image view objects upfront.

Add a swizzle-aware overload to `GERenderTarget.h` / `GECommandBuffer`:

```cpp
void bindResourceAtFragmentShader(
    SharedHandle<GETexture> & texture,
    unsigned id,
    const TextureSwizzle & swizzle = TextureSwizzle::identity());
```

The existing overloads remain unchanged — they implicitly use the identity swizzle, so this is backwards-compatible.

If `isIdentity()` returns true, backends skip any extra work and use their current code paths.

### 3. Backend mapping

#### Vulkan

When a non-identity swizzle is provided at bind time, create (or cache) a `VkImageView` with the corresponding `VkComponentMapping`:

```
TextureSwizzleChannel  →  VkComponentSwizzle
───────────────────────────────────────────────
Identity               →  VK_COMPONENT_SWIZZLE_IDENTITY
Red                    →  VK_COMPONENT_SWIZZLE_R
Green                  →  VK_COMPONENT_SWIZZLE_G
Blue                   →  VK_COMPONENT_SWIZZLE_B
Alpha                  →  VK_COMPONENT_SWIZZLE_A
Zero                   →  VK_COMPONENT_SWIZZLE_ZERO
One                    →  VK_COMPONENT_SWIZZLE_ONE
```

The swizzled `VkImageView` is written into the descriptor set in place of the texture's default view. A small LRU cache keyed on `(VkImage, TextureSwizzle)` avoids re-creating views every frame for stable swizzle configurations.

#### Metal

Call `newTextureViewWithPixelFormat:textureType:levels:slices:swizzle:` with an `MTLTextureSwizzleChannels`:

```
TextureSwizzleChannel  →  MTLTextureSwizzle
───────────────────────────────────────────────
Identity               →  MTLTextureSwizzleRed/Green/Blue/Alpha (positional)
Red                    →  MTLTextureSwizzleRed
Green                  →  MTLTextureSwizzleGreen
Blue                   →  MTLTextureSwizzleBlue
Alpha                  →  MTLTextureSwizzleAlpha
Zero                   →  MTLTextureSwizzleZero
One                    →  MTLTextureSwizzleOne
```

Note: Metal's `Identity` is position-dependent — each channel defaults to itself. The `TextureSwizzleChannel::Identity` value maps to the positionally correct `MTLTextureSwizzle` per component.

#### D3D12

Configure `Shader4ComponentMapping` on the SRV descriptor:

```
TextureSwizzleChannel  →  D3D12_SHADER_COMPONENT_MAPPING
───────────────────────────────────────────────────────────
Identity               →  D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_{0,1,2,3} (positional)
Red                    →  D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0
Green                  →  D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1
Blue                   →  D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2
Alpha                  →  D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_3
Zero                   →  D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0
One                    →  D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1
```

Composed via `D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(r, g, b, a)`.

### 4. OmegaSL shader layout descriptor extension

Extend `omegasl_shader_layout_desc` so that the compiled shader library can carry a **default swizzle** per texture resource:

```c
struct omegasl_texture_swizzle_desc {
    unsigned char r;  // 0=R, 1=G, 2=B, 3=A, 4=Zero, 5=One, 6=Identity
    unsigned char g;
    unsigned char b;
    unsigned char a;
};

struct omegasl_shader_layout_desc {
    omegasl_shader_layout_desc_type type;
    unsigned gpu_relative_loc;
    omegasl_shader_layout_desc_io_mode io_mode;
    size_t location;
    size_t offset;
    omegasl_shader_static_sampler_desc sampler_desc;
    omegasl_shader_constant_desc constant_desc;
    omegasl_texture_swizzle_desc swizzle_desc;  // NEW
};
```

This allows a future OmegaSL syntax extension (e.g. `texture2d myTex : 2 (swizzle=bgra)`) to bake a default swizzle into the compiled shader metadata. Backends that receive a layout with a non-identity `swizzle_desc` apply it when no runtime override is provided.

The runtime `bindResource` swizzle always takes precedence over the layout default when both are present.

### 5. Swizzle view caching strategy

Creating new image views / texture views per frame is expensive. The proposed caching strategy:

| Backend | Cache key | Cached object | Eviction |
|---------|-----------|---------------|----------|
| Vulkan | `(VkImage, TextureSwizzle)` | `VkImageView` | Destroy when parent `GEVulkanTexture` is destroyed |
| Metal | `(id<MTLTexture>, TextureSwizzle)` | `id<MTLTexture>` (view) | Released when parent texture is released |
| D3D12 | `(ID3D12Resource*, TextureSwizzle)` | SRV descriptor heap slot | Freed when parent texture is released |

For the identity swizzle, the existing default view/descriptor is used directly — no cache lookup.

---

## Files touched

| File | Change |
|------|--------|
| `gte/include/omegaGTE/GTEBase.h` | Add `TextureSwizzleChannel`, `TextureSwizzle` |
| `gte/include/omegaGTE/GERenderTarget.h` | Add swizzle-aware `bindResource` overloads |
| `gte/include/omegasl.h` | Add `omegasl_texture_swizzle_desc`, extend layout desc |
| `gte/src/vulkan/GEVulkanTexture.h` | Add swizzled view cache |
| `gte/src/vulkan/GEVulkan.cpp` | Map `TextureSwizzle` → `VkComponentMapping`; create swizzled views |
| `gte/src/metal/GEMetalTexture.h` | Add swizzled view cache |
| `gte/src/metal/GEMetal.mm` | Map `TextureSwizzle` → `MTLTextureSwizzleChannels`; create swizzled views |
| `gte/src/d3d12/GED3D12Texture.h` | Add swizzled SRV descriptor cache |
| `gte/src/d3d12/GED3D12.cpp` | Map `TextureSwizzle` → `Shader4ComponentMapping` |
| `gte/omegasl/src/AST.h` | (Future) Add swizzle field to `ResourceDecl` |
| `gte/omegasl/src/GLSLCodeGen.cpp` | (Future) Emit swizzle metadata in layout descs |
| `gte/omegasl/src/HLSLCodeGen.cpp` | (Future) Emit swizzle metadata in layout descs |
| `gte/omegasl/src/MetalCodeGen.cpp` | (Future) Emit swizzle metadata in layout descs |

---

## Example usage

```cpp
// Upload an RGBA texture
auto tex = heap->makeTexture(desc);
tex->copyBytes(rgbaPixels, width * 4);

// Bind it as BGRA in a fragment shader (zero-cost channel swap)
cmd.bindResourceAtFragmentShader(tex, 2, TextureSwizzle::swapRB());

// Broadcast a single-channel depth texture to all components
cmd.bindResourceAtFragmentShader(depthTex, 3, TextureSwizzle::broadcastRed());

// Identity (default) — same as today
cmd.bindResourceAtFragmentShader(tex, 2);
```

---

## Relationship to format translation doc

The existing `vulkan_metal_d3d12_format_translation_comparison.md` recommends view-level channel remapping as the preferred portable strategy for RGBA↔BGRA translation at sample time. This proposal is the direct implementation of that recommendation within OmegaGTE's abstraction layer.

---

## Open questions

1. **Should `TextureDescriptor` also carry a default swizzle?** This would bake the swizzle into the texture's primary view at creation time, avoiding per-bind overhead entirely for textures that are always sampled with the same remapping. Trade-off: less flexible, but cheaper for the common case.

2. **OmegaSL syntax timing.** The `omegasl_texture_swizzle_desc` field in the layout descriptor is proposed now so the binary format is forward-compatible. The actual OmegaSL language syntax (`texture2d myTex : 2 (swizzle=bgra)`) and code generator changes can land in a follow-up without breaking the library format.

3. **Compute shader writes.** Swizzle on UAV/storage-image writes is not proposed here. All three APIs are more restrictive about write-side remapping. If needed, a shader-side swizzle (`output.bgra = value.rgba`) is the portable path.
