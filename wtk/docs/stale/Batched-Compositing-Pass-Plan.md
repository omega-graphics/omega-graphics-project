# Batched Compositing Pass

## Problem

The compositor currently renders each layer to its own backing texture independently, then composites them to the swapchain via per-layer fullscreen quad blits inside a single Vulkan render pass (`compositeAndPresentTarget` in `RenderTarget.cpp`). This works but has structural limitations:

1. **All layers blit as fullscreen quads** regardless of their actual bounds — a 48x48 element layer occupies the same GPU fill as the 500x500 root layer.
2. **No depth/z-ordering control** — layers composite in visual tree insertion order (root, then body), with no way to reorder or interleave across widget boundaries.
3. **Per-layer fence waits** — each `BackendRenderTargetContext` signals its own fence after rendering to its backing texture, and the compositing pass must wait on all of them sequentially via `notifyCommandBuffer`.
4. **No partial update** — if only one element layer changed, we still re-blit every layer. The previous swapchain content is cleared unconditionally.
5. **Single pipeline for all blits** — the `finalCopyRenderPipelineState` (copy shader + hardcoded alpha blend) is used for every layer, including the opaque root layer where blending is wasted work.

## Proposed Architecture

Replace the current per-layer blit loop with a **batched compositing pass** that operates on a sorted, bounds-aware list of layer contributions.

### Data Model

```
CompositeEntry {
    texture      : GETexture         // committed layer texture
    srcRect      : Rect              // region within the texture that has content
    dstRect      : Rect              // position/size on the native target
    zOrder       : int               // explicit sort key
    opacity      : float             // per-layer opacity (1.0 = fully opaque)
    blendMode    : enum { Opaque, AlphaBlend }
    fence        : GEFence           // GPU fence to wait on before reading
}
```

Each `BackendRenderTargetContext::commit()` produces a `CompositeEntry` instead of just setting `hasPendingContent`. The entries accumulate on the `BackendCompRenderTarget` until the queue drains.

### Compositing Pass

On `onQueueDrained()`, for each `BackendCompRenderTarget` with pending entries:

1. **Sort** entries by `zOrder` (stable, preserving insertion order for equal z).
2. **Coalesce fences** — build a single wait list from all entry fences.
3. **Acquire** one swapchain image (`commandBuffer()` → `startRenderPass(Clear)`).
4. **For each entry** in sorted order:
   - Bind the entry's texture.
   - Set viewport/scissor to `dstRect` (not fullscreen).
   - Select pipeline: opaque (no blend) for the first opaque entry covering the full target, alpha-blend for everything else.
   - Emit a draw call (6 vertices — the existing fullscreen quad buffer works, but the vertex positions should be mapped to `dstRect` in NDC rather than always [-1,1]).
5. **End** render pass, submit, present.

### Vertex Generation for Bounded Blits

The current `finalTextureDrawBuffer` is a static fullscreen quad ([-1,1] in NDC, [0,1] in UV). For bounded blits, we need per-entry vertex data mapping `dstRect` to NDC and `srcRect` to UV.

Two options:

**A. Per-frame dynamic vertex buffer** — Allocate from the `BufferPool`, write 6 vertices per entry, bind once, use `startVertexIndex` offsets for each draw. This is simple and the buffer pool already exists.

**B. Push constants / uniform buffer** — Keep the static quad and pass `dstRect`/`srcRect` as a uniform that the vertex shader uses to transform the [-1,1] positions. Requires a new shader variant or a small uniform buffer bind per entry.

Option A is recommended — it's consistent with how `renderToTarget` already works and avoids shader changes.

### Files to Modify

| File | Change |
|---|---|
| `wtk/src/Composition/backend/RenderTarget.h` | Add `CompositeEntry` struct. Add `OmegaCommon::Vector<CompositeEntry> pendingEntries` to `BackendCompRenderTarget`. Remove `hasPendingContent` flag from `BackendRenderTargetContext`. |
| `wtk/src/Composition/backend/RenderTarget.cpp` | `commit()` builds a `CompositeEntry` with the layer's committed texture, position, and fence, and pushes it onto the owning `BackendCompRenderTarget`. Replace `compositeAndPresentTarget()` with the batched pass: sort, coalesce fences, emit bounded blits. |
| `wtk/src/Composition/backend/Execution.cpp` | `executeCurrentCommand()` passes a pointer to the owning `BackendCompRenderTarget` into `commit()` so it can push its entry. No other changes needed — `onQueueDrained()` already calls `presentAllPending()`. |
| `wtk/src/Composition/backend/VisualTree.h` | Add `Composition::Point2D` accessor to `Visual` so the compositing pass can read each layer's position for `dstRect`. |

### Opaque Root Optimization

The root layer typically covers the entire view with an opaque background. If the root entry has `opacity == 1.0` and `dstRect` covers the full native target, use `blendMode = Opaque` and skip alpha blending for that draw. This avoids blending the most expensive blit (the largest one).

To support this, create a second render pipeline state at startup — identical to `finalCopyRenderPipelineState` but with `blendEnable = VK_FALSE`. Currently all pipelines are created with `blendEnable = VK_TRUE` hardcoded in `GEVulkan.cpp:1459`. This requires either:
- A field on `RenderPipelineDescriptor` to control blending (preferred — exposes it to the abstraction layer).
- Or a second pipeline created with a patched Vulkan `VkPipelineColorBlendAttachmentState` directly (works but leaks Vulkan specifics).

### Dirty Tracking (Incremental Compositing)

Once the basic batched pass works, add dirty tracking to avoid re-compositing unchanged layers:

1. Each `BackendRenderTargetContext` keeps a **generation counter** that increments on every `commit()`.
2. `BackendCompRenderTarget` keeps a **last-presented generation** per layer.
3. On present, if all layer generations match their last-presented values, skip the compositing pass entirely (the swapchain already has the right content).
4. If only some layers changed, we still re-composite all layers (Vulkan clears the swapchain image on acquire with `LoadAction::Clear`). True partial update would require a persistent composition texture — that's a further optimization.

### Threading Considerations

The compositor scheduler runs on its own thread (`CompositorScheduler::processCommand` in `Compositor.cpp`). The compositing pass runs in `onQueueDrained()` on that same thread, so there are no new threading concerns. The `pendingEntries` vector is only written during command execution and read during `onQueueDrained()`, both on the scheduler thread.

### Migration Path

1. **Phase 1**: Replace `compositeAndPresentTarget()` internals with entry-based compositing using fullscreen quads (same visual result as today, but entries are sorted and the data flow is cleaner). No shader changes. No new pipelines.
2. **Phase 2**: Switch to bounded blits with per-frame vertex buffers. Layers smaller than the view save fill rate.
3. **Phase 3**: Add the opaque root pipeline (requires `RenderPipelineDescriptor` blending field or a second hardcoded pipeline).
4. **Phase 4**: Add generation-based dirty tracking to skip unchanged frames.

### Verification

- **BasicAppTest**: Should show a red 48x48 rectangle centered on a black 500x500 window (the original bug that motivated this work).
- **EllipsePathCompositorTest**: Three geometry shapes in an HStack — tests multi-widget, multi-layer compositing.
- **ContainerClampAnimationTest**: Animated child widget with style transitions — tests layer updates across frames.
- **TextCompositorTest**: Text rendering + UIView accent layer — tests mixed canvas draw + UIView compositing.
- **SVGViewRenderTest**: SVG rendering through a dedicated view type.

Run each test visually and confirm that all layers appear composited in the correct order with proper alpha blending.
