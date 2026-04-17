# Vulkan vs Metal vs D3D12: RGBA8/BGRA8 Format Translation

This note compares how **Vulkan**, **Metal**, and **D3D12** handle format conversion or channel remapping for operations like image copies, blits, shader sampling, and render-target usage.

The motivating example is:

- `RGBA8Unorm` ↔ `BGRA8Unorm`

In other words: can the API implicitly handle an R/B channel swap, or do you need a view swizzle or shader pass?

---

## 1) API behavior comparison

| Operation / case | Vulkan | Metal | D3D12 |
|---|---|---|---|
| **Raw texture copy** | **No general translation.** Copy commands are copy operations; do not assume automatic R/B swap. Blits are the conversion path, not copies. | **No.** `MTLBlitCommandEncoder` texture-to-texture copies require the destination texture to have the **same pixel format and sample count** as the source. | **No general translation.** `CopyResource` is copy-only; formats must be identical or in limited compatible/reinterpret groups, and the underlying bits are **not converted**. |
| **Blit / scaled copy with format conversion** | **Yes, often.** `vkCmdBlitImage` supports format conversion for many color formats; unorm/snorm/scaled/packed-float conversion goes through float, with defined sRGB handling. | **Not as a format-converting blit.** Metal blit-copy APIs still require matching texture pixel formats for texture-to-texture copies. | **Not via copy APIs.** D3D12 copy operations are not a general format-converting blit path; compatible reinterpret copies are limited and do not do channel-value conversion. |
| **`RGBA8Unorm` → `BGRA8Unorm` specifically** | **Usually yes via blit, not copy.** Vulkan format-converting blits are the closest thing to an API-managed translation here. | **Not via blit copy.** You would normally solve this with a **texture view / swizzle** or a shader pass, not a texture-copy blit. | **Not via copy.** `RGBA8` ↔ `BGRA8` is not the kind of typed reinterpret copy D3D12 copy ops are for; use shader/view-based handling instead. |
| **View-based reinterpretation** | **Yes, with image views** where allowed by format/view rules; this is separate from copy/blit behavior. | **Yes.** `makeTextureView(...)` creates a texture view reinterpreting the data with a **compatible** pixel format. | **Yes, in a limited sense.** Views can use format reinterpretation within DXGI typing rules; SRVs also expose component mapping controls. |
| **Channel swizzle at sample/read time** | **Yes.** Image views support component swizzles, so sampling can remap channels without rewriting the texture. | **Yes.** Metal texture views support a swizzle pattern, and Metal documents swizzle as applying when you read/sample pixels from the texture. | **Yes.** D3D12 SRVs include `Shader4ComponentMapping`, which controls how components are routed to shader reads. |
| **Render target / storage image style usage** | Usually **strict**; do not expect implicit translation just because formats are “similar.” Vulkan is explicit here. | Usually **strict**; use the format the pipeline/resource expects, or use a view/shader path explicitly. | Usually **strict**; pipeline/render-target formats are explicit and validated against the PSO/resource view formats. |

---

## 2) Best implementation choice by use case

| Use case | Vulkan | Metal | D3D12 | Recommended portable strategy |
|---|---|---|---|---|
| **Upload a CPU image into a GPU texture** | Prefer matching destination format. | Prefer matching destination format. | Prefer matching destination format. | Convert on CPU if practical, or upload as authored format and handle remap via view/shader. |
| **Fast exact GPU copy** | Use `vkCmdCopyImage*` when formats/layout rules allow. | Use a blit encoder copy when formats match. | Use `CopyResource` / `CopyTextureRegion` when compatible. | Treat this as a **bit-preserving copy**, not a conversion step. |
| **Resize + convert texture data** | `vkCmdBlitImage` is the most direct API path. | Use a render/shader pass. | Use a render/shader pass. | For portability, plan on a **shader pass**. |
| **Generate mipmaps** | Often via blits where supported, otherwise shader. | Commonly via blit generation or shader workflows depending on engine design. | Often shader or copy/render workflow. | Use backend-specific optimized paths, but keep a shader fallback. |
| **Sample an `RGBA8` texture as `BGRA8`** | Use image-view component swizzle. | Use a texture view with swizzle. | Use SRV component mapping. | Best portable choice: **view-level channel remap** if the backend supports the needed view rules. |
| **Need a new physical texture with channels actually rewritten** | Blit may work for many color formats; shader is safest. | Use a shader pass. | Use a shader pass. | Best portable choice: **shader pass** writing into a new texture. |
| **Render target / postprocess pass** | Use the actual attachment format expected by the pipeline. | Same. | Same. | Keep RT formats explicit. Avoid relying on implicit API translation. |
| **Storage image / UAV style access** | Use exact intended format; do not rely on similar-format reinterpretation. | Same principle. | Same principle. | Use exact formats or explicit shader conversion. |
| **Swapchain / presentation interop** | Usually create resources in the swapchain-compatible format, or use a final conversion pass. | Same. | Same. | Convert explicitly in the final pass if your internal format differs from presentation format. |
| **Cross-backend renderer policy** | Vulkan has the richest built-in blit conversion path here. | More explicit. | More explicit. | Design for **view swizzle first**, **shader pass second**, and treat Vulkan blit conversion as an optimization. |

---

## 3) Practical rules of thumb

### If you only need the texture to *read* as BGRA instead of RGBA
Use a **view-level channel remap**:

- **Vulkan:** image view component swizzle
- **Metal:** texture view + swizzle
- **D3D12:** SRV component mapping

This avoids rewriting texture memory and is usually the cleanest solution.

### If you need a new texture whose bytes/components are actually rewritten
Use a **shader pass** as the portable answer.

- Vulkan can sometimes do this through a **blit with format conversion**.
- Metal and D3D12 generally should not be expected to do this through copy/blit APIs.

### If you are doing a plain copy
Treat copy operations as:

- **copy bits / texels**
- **not a general format translator**

That rule will keep your renderer design correct on all three APIs.

---

## 4) Recommended backend policy for an engine

A solid cross-platform policy is:

1. **Prefer exact format matches** for copies, render targets, storage images, and presentation paths.
2. **Use view swizzles/component mapping** when the only problem is channel order at sampling time.
3. **Use a shader pass** when you need a guaranteed portable physical conversion.
4. **Use Vulkan blit conversion as an optimization**, not as the core abstraction your renderer depends on.

That gives you a model that works consistently across:

- Vulkan
- Metal
- D3D12

while still letting Vulkan take advantage of its stronger blit-conversion behavior.

---

## 5) Short takeaway

- **Vulkan:** copy is copy; **blit can convert**; view swizzle is explicit and useful.
- **Metal:** blit copies generally want matching pixel formats; use **texture views + swizzle** or a shader.
- **D3D12:** copy is copy/reinterpret in narrow cases; use **SRV component mapping**, compatible views, or a shader pass.

For `RGBA8Unorm` ↔ `BGRA8Unorm`, the best portable answer is:

- **sample-time remap:** use a **view swizzle / component mapping**
- **physical converted texture:** use a **shader pass**
- **Vulkan-only optimization:** a **blit** may handle the conversion directly
