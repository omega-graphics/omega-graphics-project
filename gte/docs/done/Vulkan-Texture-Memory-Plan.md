# Vulkan Texture Memory & `makeTexture` Hardening Plan

## Goal

Replace the LINEAR-tiling shortcut that currently powers `ToGPU` / `FromGPU`
on Vulkan with a staging-buffer pattern that mirrors what D3D12 already does
(`gte/src/d3d12/GED3D12.cpp:2008–2027` — the `cpuSideRes` UPLOAD/READBACK
companion resource). Round out the remaining `makeTexture` rough edges
identified in the per-usage audit while we're in the file.

After this lands, `ToGPU` / `FromGPU` textures use OPTIMAL tiling on every
device, support arbitrary formats / mip chains / sample counts that
LINEAR can't cover, and `copyBytes` / `getBytes` work on discrete and
integrated GPUs without a fast / slow path split.

## Current State

`gte/src/vulkan/GEVulkan.cpp` `makeTexture` was just hardened (see
[done/Vulkan-Texture-Memory-Audit](done/) — the per-usage audit). The
remaining concession is **LINEAR tiling for `ToGPU` / `FromGPU`**:

- Image is allocated `LINEAR` + `HOST_VISIBLE`.
- `GEVulkanTexture::copyBytes` / `getBytes` map the image's allocation
  directly via `vmaMapMemory` and `memcpy`.
- A `vkGetPhysicalDeviceFormatProperties` guard refuses unsupported
  formats up front with a clear diagnostic.

This works for every current caller (`BitmapTextureCache`,
`HarfBuzzTextRect`, `CTFontEngine`, `DWriteFontEngine`) because they all
upload single-mip color bitmaps and sample them. It does not work for:

| Scenario | Why LINEAR fails |
|---|---|
| Mipmapped uploads | LINEAR images are single-mip on virtually every driver |
| Multisampled uploads | `samples > 1` requires OPTIMAL |
| Compressed formats (BC, ASTC, ETC) | `linearTilingFeatures` is empty for block-compressed formats on every desktop driver |
| Optimal-only formats | Some YUV / planar / sRGB variants are OPTIMAL-only |
| Render-then-readback in one descriptor | Render target → readback wants OPTIMAL for the render side |

D3D12 sidesteps every one of these by keeping the image device-local and
adding a small companion buffer in an UPLOAD/READBACK heap. The same
pattern is the canonical answer on Vulkan.

## Why this matters

- **Format coverage.** As soon as any caller wants a compressed-texture
  upload (texture asset streaming, GPU-baked atlases, compressed font
  glyphs), the LINEAR path will refuse it via the format guard. The
  staging-buffer path doesn't care about `linearTilingFeatures` — only
  the much wider `optimalTilingFeatures` set.
- **Performance.** LINEAR-tiled images sample slower on most drivers
  (no swizzling, cache-unfriendly). For long-lived sampled textures
  (sprite atlases, UI bitmaps reused across many frames) the difference
  is real even at small sizes. Texture upload happens once; sampling
  happens per frame.
- **D3D12 / Metal parity.** Both other backends already do staging
  internally. Keeping Vulkan on a divergent path means every cross-
  backend perf bug ("works on Metal, slow on Vulkan") points back here.
- **Future MSAA / compute-write paths.** Anything that wants to render
  into a texture and then have the CPU read it back (debug captures,
  GPU-driven UI export, screenshot tooling) needs OPTIMAL on the GPU
  side and a readback buffer on the CPU side. Today it can't be
  expressed at all.

## Non-Goals

- **Async upload.** This plan keeps `copyBytes` synchronous (record →
  submit → wait), matching D3D12's behavior. An async-upload command
  queue is a separate piece of work that belongs alongside the Direct
  GPU I/O plan (`Direct-IO-Implementation-Plan.md`), not bundled here.
- **Public API change.** `GETexture::copyBytes` / `getBytes` keep their
  signatures. The staging buffer is an implementation detail of
  `GEVulkanTexture`. No call site re-writes.
- **LINEAR removal.** The LINEAR path stays as a fast-path opt-in for
  small one-shot uploads where the synchronous staging cost is the
  dominant factor. The default for `ToGPU`/`FromGPU` switches to
  staging; LINEAR becomes a `TextureDescriptor` flag (see Open Q1).
- **Replacing the format guard.** The
  `vkGetPhysicalDeviceFormatProperties` guard moves over to checking
  `optimalTilingFeatures` instead of `linearTilingFeatures` and stays
  as a sanity check. Diagnostics improve in passing.

---

## Design

### `GEVulkanTexture` gains a staging buffer

```cpp
class GEVulkanTexture : public GETexture {
    // ... existing fields ...

    /// Companion HOST_VISIBLE buffer for ToGPU / FromGPU textures.
    /// Allocated alongside the image when the descriptor's usage is
    /// ToGPU or FromGPU; otherwise VK_NULL_HANDLE / nullptr. The size
    /// is `bytesPerRow(format, width) * height * arrayLayers * mipChain`,
    /// rounded up per the device's optimal buffer-image transfer
    /// granularity.
    VkBuffer       stagingBuffer        = VK_NULL_HANDLE;
    VmaAllocation  stagingAlloc         = nullptr;
    VkDeviceSize   stagingSize          = 0;

    /// Per-mip per-layer offsets into stagingBuffer. Populated at
    /// creation time using vkGetImageSubresourceLayout-equivalent
    /// math (or, for the buffer side, tightly-packed rows + slices).
    OmegaCommon::Vector<VkBufferImageCopy> stagingRegions;
};
```

The image itself is `OPTIMAL` + `GPU_ONLY`, with usage flags as written
in the per-usage audit (`SAMPLED | TRANSFER_SRC | TRANSFER_DST` for
`ToGPU`, similarly for `FromGPU`).

### `copyBytes` flow (ToGPU)

```cpp
void GEVulkanTexture::copyBytes(void *bytes, size_t bytesPerRow){
    // 1. Map the staging buffer.
    void *ptr = nullptr;
    vmaMapMemory(engine->memAllocator, stagingAlloc, &ptr);

    // 2. Lay the source bytes out per-mip/per-layer to match
    //    stagingRegions. For the common single-mip / single-layer
    //    case this is a single tightly-packed memcpy.
    layoutSourceBytesIntoStaging(ptr, bytes, bytesPerRow);

    vmaUnmapMemory(engine->memAllocator, stagingAlloc);

    // 3. Record a one-shot transfer command buffer that:
    //    a. Transitions image: UNDEFINED -> TRANSFER_DST_OPTIMAL.
    //    b. Issues vkCmdCopyBufferToImage(stagingBuffer -> image,
    //       stagingRegions).
    //    c. Transitions image: TRANSFER_DST_OPTIMAL ->
    //       SHADER_READ_ONLY_OPTIMAL (the layout ToGPU images expect
    //       at first bind).
    //    d. Submits to a transfer-capable queue.
    //    e. Waits on a fence (synchronous, matching the call's
    //       existing contract).
    engine->submitImmediateUploadFromStaging(*this);
}
```

`getBytes` is the mirror: image transition → `vkCmdCopyImageToBuffer`
into staging → fence wait → map/copy out.

### `submitImmediateUploadFromStaging` lives on the engine

A new private method on `GEVulkanEngine` that:

1. Pulls a small dedicated `VkCommandPool` + `VkCommandBuffer` from a
   round-robin ring (created once at engine init; sized for the
   typical concurrent upload count — 4 is sufficient for what WTK
   does today).
2. Records the transfer commands.
3. Submits to the **transfer queue family** if available (some
   discrete GPUs expose a dedicated DMA queue), else the graphics
   queue.
4. Waits on a `VkFence` from a small pool. The fence is reset and
   returned to the pool on completion.

This is intentionally *not* using the engine's main retention queue or
the user's `GECommandBuffer` infrastructure — uploads are
implementation-detail copies that happen before the texture is ever
visible to user code, so they don't need to participate in fence-gate
tracking. They do need to complete before `copyBytes` returns, hence
the synchronous wait.

### Existing LINEAR fast path becomes opt-in

Add to `TextureDescriptor`:

```cpp
struct TextureDescriptor {
    // ... existing fields ...

    /// When true, bypass the staging-buffer upload pattern and try to
    /// allocate the image directly in HOST_VISIBLE memory with LINEAR
    /// tiling. Faster `copyBytes` (no command-buffer round-trip) at
    /// the cost of restricted format support and no mips / MSAA.
    /// Backends that don't have a meaningful LINEAR concept ignore
    /// this hint. Defaults to false; the previous Vulkan-only
    /// behavior matched `directHostMap = true`.
    bool directHostMap = false;
};
```

When `directHostMap` is true and the format passes the LINEAR feature
check, the existing LINEAR code path runs. Otherwise the staging
buffer is allocated. WTK's font / bitmap callers can flip the flag if
profiling shows the synchronous upload is hot for their workload.

---

## Implementation phases

### Phase 1 — Engine infrastructure
- `GEVulkanEngine`: add transfer command-pool ring + fence pool.
  Initialize at `GEVulkanEngine::init` time. Tear down before queue
  destruction.
- Helper `submitImmediateUploadFromStaging(GEVulkanTexture &)` — pure
  internal, no header surface.
- Choose transfer queue family at engine creation (capability check
  is already done elsewhere; just record the chosen index).

### Phase 2 — `GEVulkanTexture` staging fields
- Add `stagingBuffer`, `stagingAlloc`, `stagingSize`,
  `stagingRegions` to the texture class.
- Helper `computeStagingLayout(descriptor)` that fills
  `stagingRegions` for an arbitrary mip chain / array length /
  format. Tightly-packed rows for the buffer side, image-natural
  layout for the image side via `VkBufferImageCopy`.
- Destructor releases the buffer alongside the image (gated on the
  same retention queue so an in-flight upload doesn't free the
  buffer mid-copy).

### Phase 3 — Switch ToGPU / FromGPU paths
- `makeTexture` for `ToGPU`: revert to OPTIMAL tiling, add the
  staging buffer. Keep the format guard but check
  `optimalTilingFeatures` for `SAMPLED_IMAGE_BIT`.
- Same for `FromGPU` with `optimalTilingFeatures` checking
  `TRANSFER_SRC_BIT`.
- `copyBytes` / `getBytes` re-routed through staging. The
  `vmaMapMemory`-on-image path goes away unless `directHostMap` is
  set.

### Phase 4 — `directHostMap` opt-in
- Add the field to `TextureDescriptor`.
- LINEAR path stays, gated on the new flag. No caller migrates yet —
  the staging path is the new default and is correct for all current
  callers. Profiling later can decide which (if any) callers want to
  flip the flag.

### Phase 5 — Documentation
- Update the per-usage table in CLAUDE-relevant sections (this
  document moves to `done/` once Phase 4 ships).
- Note in `OmegaSL-Reference.md` (or wherever resource-binding is
  user-facing) that `ToGPU` is now staging-backed by default.

---

## Open Questions

1. **Should `directHostMap` exist at all, or always staging?** The
   staging path is correct for every current caller. The LINEAR fast
   path is real but its perf benefit only shows up when `copyBytes`
   is on a hot path (per-frame texture uploads). None of the current
   callers do per-frame uploads; the texture cache reuses its
   allocations. Recommendation: ship without the flag; add it only
   if profiling reveals a real hot spot. The flag is otherwise a
   tempting footgun (callers will set it because "faster" without
   thinking through the format-coverage cost).

2. **Transfer queue vs. graphics queue.** Discrete GPUs expose a
   transfer-only queue family that's faster for pure copies and
   doesn't contend with rendering. Integrated GPUs typically don't.
   The engine already enumerates queue families; the question is
   whether to pick one transfer queue at init or fall back to the
   graphics queue when the device doesn't expose one. Recommendation:
   prefer transfer if available, fall back transparently. Either way
   the fence-wait semantics are identical.

3. **Mip chain population on `copyBytes`.** Today `copyBytes` takes
   `(bytes, bytesPerRow)` — a single mip's worth. Mipmapped uploads
   need either (a) a new `copyBytes(level, layer, bytes,
   bytesPerRow)` overload, or (b) a separate
   `generateMipmaps(texture)` blit-pass call (which exists today).
   Recommendation: leave the single-mip overload alone; document
   that `copyBytes` writes mip 0 only and the caller is expected to
   call `generateMipmaps` afterwards. Multi-mip CPU uploads can be
   added later via a new overload without breaking anyone.

4. **Compressed-format upload.** Block-compressed textures (BC, ASTC)
   have a `bytesPerBlock` instead of `bytesPerRow`-per-pixel. The
   current `copyBytes` signature doesn't carry that. If/when WTK
   wants compressed uploads, we add a sibling
   `copyCompressedBytes(blocksPerRow, ...)` and route it through the
   same staging machinery. Out of scope for this plan; flagged here
   so the staging-region computation in Phase 2 doesn't bake in a
   uncompressed-only assumption.

---

## Files touched

| File | Change |
|---|---|
| `gte/src/vulkan/GEVulkan.h` | Add transfer-pool / fence-pool fields to `GEVulkanEngine`. |
| `gte/src/vulkan/GEVulkan.cpp` | Phase 1 init / teardown; revert ToGPU / FromGPU tiling to OPTIMAL; widen format guard to `optimalTilingFeatures`. |
| `gte/src/vulkan/GEVulkanTexture.h` | Phase 2 fields and helper signature. |
| `gte/src/vulkan/GEVulkanTexture.cpp` | Phase 3 `copyBytes` / `getBytes` re-routing; staging-region computation; destructor cleanup. |
| `gte/include/omegaGTE/GETexture.h` | Phase 4 `directHostMap` flag. |
| `gte/docs/Vulkan-Texture-Memory-Plan.md` | This file — moves to `done/` after Phase 4 ships. |

---

## Related follow-ups (not part of this plan)

The per-usage audit identified two more deferred items that are
*not* gated on this plan and can land independently:

1. **Depth / stencil texture formats.** `TexturePixelFormat` has no
   depth formats today; `RenderTargetAndDepthStencil` is currently
   identical to `RenderTarget` because of that. When depth formats
   are added, the `RenderTargetAndDepthStencil` arm needs to branch
   on the format family and emit `DEPTH_STENCIL_ATTACHMENT_BIT`
   instead of `COLOR_ATTACHMENT_BIT`. The TODO comment in
   `GEVulkan.cpp:makeTexture` marks the spot. Same work item on D3D12
   (`isDSV` already exists) and Metal (already format-driven).

2. **`getOrCreateSwizzledView` aspect mask.** Hard-coded to
   `VK_IMAGE_ASPECT_COLOR_BIT`. Correct today because every supported
   pixel format is color, but needs to derive from the format the
   moment depth / stencil formats land. Co-lands with item 1.

3. **Texture `defaultSwizzle` ↔ shader layout `swizzle_desc`
   composition.** When both are non-identity, the layout-fallback
   path in `resolveEffectiveSwizzle` allocates a fresh swizzled view
   instead of recognizing that `img_view` already has the texture's
   default swizzle baked in. Correctness is fine (the new view has
   the right channels), but it's a redundant `VkImageView` per
   texture per shader. Tighten the cache to short-circuit when the
   resolved swizzle equals the texture's default, which would
   otherwise reuse `img_view`.
