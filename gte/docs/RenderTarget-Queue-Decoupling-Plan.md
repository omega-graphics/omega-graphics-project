# GERenderTarget / Command-Queue Decoupling Plan

## Goal

Remove the `GERenderTarget::CommandBuffer` wrapper and the per-render-target
internal `GECommandQueue`. The application owns command queues and command
buffers. A render target is data the user attaches to a render pass via the
render-pass descriptor — nothing more.

After the change:

```cpp
// User creates ONE queue (and reuses it across frames / targets).
auto queue = engine->makeCommandQueue(64);

// User creates the targets.
auto rt   = engine->makeNativeRenderTarget(desc, queue);   // see "present queue" below
auto offs = engine->makeTextureRenderTarget(offDesc);      // no queue needed

// Per frame:
auto cb = queue->getAvailableBuffer();

GERenderPassDescriptor pass {};
pass.nRenderTarget = rt.get();             // <-- render target lives on the pass
pass.colorAttachments.push_back(...);
cb->startRenderPass(pass);
cb->setRenderPipelineState(...);
cb->drawPolygons(...);
cb->finishRenderPass();

queue->submitCommandBuffer(cb);
queue->commitToGPU();
rt->present();                              // native targets only
```

## Why now

`makeNativeRenderTarget` / `makeTextureRenderTarget` currently call
`makeCommandQueue(64)` or `makeCommandQueue(100)` internally
([gte/src/d3d12/GED3D12.cpp:1418, :1467](../src/d3d12/GED3D12.cpp),
[gte/src/metal/GEMetal.mm:1013, :1178](../src/metal/GEMetal.mm),
[gte/src/vulkan/GEVulkanRenderTarget.cpp:60, :227](../src/vulkan/GEVulkanRenderTarget.cpp)).
**Every render target spawns its own device queue.** Composition systems
(WTK creates many texture render targets for offscreen layers and
post-processing) burn through the platform queue budget instantly.

Command queues are not infinite:

| Backend | What `GECommandQueue` maps to | Practical limit |
|---|---|---|
| **Metal** | `id<MTLCommandQueue>` | No documented hard cap, but Apple guidance treats it as heavyweight: create a small fixed pool and reuse. Each queue carries scheduler overhead. |
| **D3D12** | `ID3D12CommandQueue` | No documented hard cap, but each queue is a kernel-scheduled GPU engine. Vendor guidance (NV/Intel/AMD) is to keep total ≤ ~4–8 (typically 1 direct + 1 copy + 1 compute). Each is also tracked by GPU-PIX/PIX-on-Windows and consumes WDDM scheduling slots. |
| **Vulkan** | `VkCommandPool` (today) submitting to a `VkQueue` retrieved from a queue family | A `VkQueue` cannot be created — it must be retrieved from a family whose `queueCount` is fixed by the driver (often 1 graphics on NV consumer, 1–2 compute, 1 transfer; AMD typically more; SwiftShader/lavapipe expose 1). The pool wrapper hides this today, but every pool still binds to one underlying `VkQueue` index. |

The current "one queue per render target" model is wasteful on Metal/D3D12
and **fails outright on Vulkan** when a feature opens a sustained number of
texture targets. The right model: the user picks how many physical queues
exist; render targets are bound to a specific queue *by reference* only when
the platform requires it (swap chain present), and otherwise consume nothing.

## Present-queue caveat (D3D12 / Vulkan)

A native render target is not queue-free. It owns a swap chain, and on
D3D12/Vulkan the swap chain is bound to one queue at creation time:

- **D3D12**: `IDXGIFactory::CreateSwapChainForHwnd(pCommandQueue, ...)` —
  the swap chain remembers `pCommandQueue` and `Present` schedules on it.
  Passing a different queue at present time silently breaks tearing/sync.
- **Vulkan**: `vkQueuePresentKHR(presentQueue, ...)` — `presentQueue` must
  belong to a family with present support for the surface. Drivers expose
  one such family.
- **Metal**: `[mtlCommandBuffer presentDrawable:]` — the CB just has to be
  on *some* `MTLCommandQueue` the device owns. No swap-chain binding.

So `GENativeRenderTarget` cannot be queue-agnostic. It must hold a non-owning
reference to the queue used to create the swap chain. Texture render targets
have **no queue dependence at all** — they're a `GETexture` plus framebuffer
metadata; whichever queue the user submits a buffer to that writes them is
fine (subject to barriers / fences the user controls).

## API delta

### `GERenderTarget` (gte/include/omegaGTE/GERenderTarget.h)

Delete:
- `class GERenderTarget::CommandBuffer` (nested class, lines 68–272).
- `GERenderTarget::commandBuffer()`.
- `GERenderTarget::submitCommandBuffer(...)` (both overloads).
- `GERenderTarget::notifyCommandBuffer(...)`.
- `GERenderTarget::nativeCommandQueue()`.
- The `RenderPassDesc` nested struct **stays**, but enums `PolygonType` and
  `IndexType` migrate to `GECommandBuffer` (their natural home).

`GENativeRenderTarget` keeps platform-specific helpers (`getSwapChain`,
`resizeSwapChain`, `waitForGPU`, `waitForFence`, `pixelFormat`) and gains:

```cpp
class GENativeRenderTarget : public GERenderTarget {
public:
    /// Queue the swap chain was created against. Required for present.
    /// Non-owning reference held internally; user keeps the queue alive.
    virtual SharedHandle<GECommandQueue> presentQueue() const = 0;

    /// Submit the engine's internal "transition to PRESENT + Present" work
    /// on the present queue. Replaces `commitAndPresent()`.
    /// Caller has already submitted draw work to the present queue.
    virtual void present() = 0;
};
```

`GETextureRenderTarget` loses every queue method. It becomes:

```cpp
class GETextureRenderTarget : public GERenderTarget {
public:
    virtual SharedHandle<GETexture> underlyingTexture() = 0;
    /// No-op on Metal; D3D12/Vulkan use this to track that pending writes
    /// to the underlying texture have been issued so a future use on a
    /// different queue can fence correctly. Optional.
    virtual void markWritten() {}
};
```

(`commit()`, `waitForGPU()`, `signalFence()` go away — the user already has
the queue and can call `queue->commitToGPU()`, `queue->commitToGPUAndWait()`,
`queue->signalFence()` directly.)

### `GECommandBuffer` (gte/include/omegaGTE/GECommandQueue.h)

Promote everything that `GERenderTarget::CommandBuffer` exposed today to the
**public** surface of `GECommandBuffer`. The methods all already exist as
private virtuals on `GECommandBuffer` (lines 95–135) — they're just hidden
behind the wrapper. Make them public.

Add the enums that lived on the nested class:

```cpp
class GECommandBuffer : public GTEResource {
public:
    enum class PolygonType : uint8_t { Triangle, TriangleStrip, Line, LineStrip, Point };
    enum class IndexType   : uint8_t { UInt16, UInt32 };
    // ... existing methods, now all public ...
};
```

Move `bindMesh` / `drawMesh` (currently in `GERenderTarget::CommandBuffer`,
[gte/src/GERenderTarget.cpp:117–152](../src/GERenderTarget.cpp)) to
`GECommandBuffer` — they were never render-target-specific, only happened
to live there because of the wrapper.

`startRenderPass` already takes a `GERenderPassDescriptor` whose
`nRenderTarget` / `tRenderTarget` fields name the target. The wrapper's
job of synthesizing this descriptor from a `RenderPassDesc` plus the
back-pointer goes away — the user fills the descriptor directly.

### Engine factories (gte/include/omegaGTE/GE.h)

```cpp
// Native targets must be tied to a queue (the swap chain's present queue
// on D3D12 / Vulkan). On Metal the queue is recorded but only used so
// that internal "present" command-buffer encoding can find a queue.
virtual SharedHandle<GENativeRenderTarget>
    makeNativeRenderTarget(const NativeRenderTargetDescriptor & desc,
                            SharedHandle<GECommandQueue> presentQueue) = 0;

// Texture targets stay queue-free.
virtual SharedHandle<GETextureRenderTarget>
    makeTextureRenderTarget(const TextureRenderTargetDescriptor & desc) = 0;
```

The factory signature change is a hard break — every caller must thread a
queue through. That's intentional: it forces the user to think about how
many queues their app actually needs.

### `GERenderPassDescriptor` (already exists, gte/include/omegaGTE/GECommandQueue.h:59)

No structural change; this struct already carries `nRenderTarget` /
`tRenderTarget` plus color/depth attachments. After the wrapper is removed,
this is the **only** way render-target-on-pass association happens.

## Implementation phases

### Phase 1 — Header carve-out (no behavior change yet)

1. In `GERenderTarget.h`, mark `class CommandBuffer` deprecated. Move
   `PolygonType` / `IndexType` to `GECommandBuffer` and re-export the old
   names as `using` aliases inside the wrapper for one release.
2. In `GECommandQueue.h`, move every render-pass / compute-pass / blit-pass
   method on `GECommandBuffer` from `private` to `public`. Drop the
   `friend class GERenderTarget::CommandBuffer`.
3. Add the new public methods (`bindMesh`, `drawMesh`, completion handler
   re-exposure if needed) on `GECommandBuffer`.

This phase compiles and runs unchanged — the wrapper still works, but the
underlying API is now usable directly.

### Phase 2 — Backend factories accept an external queue

Change every `GE*Engine::makeNativeRenderTarget` to take
`SharedHandle<GECommandQueue> presentQueue` and stop calling
`makeCommandQueue(64)` internally. Same for the texture factory: drop the
internal queue, the constructor no longer takes one.

Backend-specific work:

| File | Change |
|---|---|
| [`gte/src/d3d12/GED3D12.cpp:1418, 1467`](../src/d3d12/GED3D12.cpp) | `createSwapChainFromHWND` already needs the queue — pass the user's. Drop the `makeCommandQueue(64)` line. `GED3D12TextureRenderTarget` ctor drops the queue param. |
| [`gte/src/metal/GEMetal.mm:1013, 1178`](../src/metal/GEMetal.mm) | Drop the local `makeCommandQueue(64)`. `GEMetalNativeRenderTarget` records the user's queue (still needed for present-CB encoding). `GEMetalTextureRenderTarget` drops the queue param. |
| [`gte/src/vulkan/GEVulkanRenderTarget.cpp:60, 227`](../src/vulkan/GEVulkanRenderTarget.cpp) | Drop the in-ctor `makeCommandQueue(100)` calls. Native target stores the user's queue (used to acquire the swap-chain image and present). Texture target stops storing one. |

Backend RT classes lose: their `commandBuffer()` override, both
`submitCommandBuffer` overloads, `notifyCommandBuffer`, `nativeCommandQueue`,
`commit()` (texture), and replace `commitAndPresent()` with `present()`.
The `present()` body for each backend:

- **Metal**: encodes `[mtlCB presentDrawable:]` on the *last* command buffer
  the user submitted to `presentQueue` and then calls
  `presentQueue->commitToGPU()`. (Today this is what
  [`GEMetalRenderTarget.mm:59`](../src/metal/GEMetalRenderTarget.mm) does;
  same logic, the queue identity comes from the user not the target.)
- **D3D12**: emits the `RENDER_TARGET → PRESENT` barrier on a small
  internally-managed command buffer obtained from `presentQueue`,
  `presentQueue->commitToGPU()`, then `swapChain->Present1`. The barrier
  CB cannot live on a different queue from the swap chain.
- **Vulkan**: signals/queues `vkQueuePresentKHR` on the queue the
  swap chain was acquired against. Today this is hidden inside
  `commitToGPUPresent`; expose it through `present()`.

### Phase 3 — Delete the wrapper

1. Remove `GERenderTarget::CommandBuffer` from
   [`GERenderTarget.h`](../include/omegaGTE/GERenderTarget.h) and
   [`GERenderTarget.cpp`](../src/GERenderTarget.cpp).
2. Update the WTK composition backend
   ([`wtk/src/Composition/backend/RenderPass.cpp`](../../wtk/src/Composition/backend/RenderPass.cpp),
   [`RenderTarget.cpp`](../../wtk/src/Composition/backend/RenderTarget.cpp),
   [`Texture.cpp`](../../wtk/src/Composition/backend/Texture.cpp)):
   - Hold `SharedHandle<GECommandQueue>` on the compositor (one per
     window — same queue used for `nativeTarget_` is reused for
     `preEffectTarget` and any other texture target the compositor owns).
   - Replace every `target->commandBuffer()` with `queue->getAvailableBuffer()`.
   - Replace every `target->submitCommandBuffer(cb)` with `queue->submitCommandBuffer(cb)`.
   - Replace `nativeTarget_->commitAndPresent()` with
     `queue->commitToGPU(); nativeTarget_->present();`
     (or equivalent — see Metal/D3D12/Vulkan note above).
   - Replace `preEffectTarget->commit()` with `queue->commitToGPU()`.
   - Replace `target->notifyCommandBuffer(cb, fence)` with
     `queue->notifyCommandBuffer(cb, fence)`.
3. Update tests:
   [`gte/tests/metal/2DTest/main.mm`](../tests/metal/2DTest/main.mm),
   [`gte/tests/vulkan/2DTest/main.cpp`](../tests/vulkan/2DTest/main.cpp),
   [`gte/tests/vulkan/BlitTest/main.cpp`](../tests/vulkan/BlitTest/main.cpp),
   [`gte/tests/directx/2DTest/main.cpp`](../tests/directx/2DTest/main.cpp).
   Each test creates one queue, threads it through `makeNativeRenderTarget`,
   uses it to allocate buffers, and presents through it.
4. Update [`gte/docs/API.rst`](API.rst) sections referring to
   `renderTarget->commandBuffer()` (lines 1069, 1218).
5. Update WTK plan docs that reference the old idiom
   ([Direct-To-Drawable-And-SDF-Plan.md, Render-Execution-Efficiency-Plan.md](../../wtk/docs/))
   — short follow-up, low priority, but flag them so they stop being copy-paste templates.

### Phase 4 — TEContext queue plumbing

[`gte/src/d3d12/D3D12TEContext.cpp:364, 402`](../src/d3d12/D3D12TEContext.cpp) and
[`gte/src/metal/MetalTEContext.mm:224, 258`](../src/metal/MetalTEContext.mm) currently
read `target->nativeCommandQueue()` to get the right ID3D12CommandQueue /
MTLCommandQueue for tessellation work. They have to instead receive a queue
explicitly. Two options:

1. **`createTEContextFromNativeRenderTarget(rt, queue)`** — caller passes
   the same queue they're using for the target.
2. **`createTEContext(queue)`** — TEContext is no longer "tied" to a render
   target at all; it just holds a queue. Render targets get passed per call.

Option 2 is the cleaner end state and matches the rest of this refactor.

## Open design questions

1. **Should `GENativeRenderTarget::present()` be on the queue instead?**
   Argument for: the present is conceptually a queue submission, and
   `queue->commitToGPUAndPresent(rt)` reads naturally. Argument against:
   different render targets present differently (DXGI vs `vkQueuePresentKHR`
   vs `presentDrawable:`); polymorphic dispatch on `rt` is cleaner than
   on `queue`. **Recommendation: keep `present()` on the render target.**

   ANWSER: YES

2. **How does the user know which queue a swap chain demands?**
   On D3D12 the swap chain is bound at creation, so passing the queue
   into `makeNativeRenderTarget` is unambiguous. On Vulkan the queue
   family must support present for that surface — the engine should
   validate this at creation time and fail fast. Add a validation pass
   in `makeNativeRenderTarget`: if `presentQueue`'s family doesn't
   match the surface's present-capable family, return `nullptr` and log.

   ANSWER: YES

3. **Texture render target → fence handoff between queues.**
   When the user renders to a texture target on queue A and samples it
   on queue B, the user is responsible for the fence today (we already
   expose `signalFence` / `notifyCommandBuffer` on the queue). Keep that
   contract — the texture render target doesn't need to know about queues.

   ANWSER: keep signalFence, remove notifyCommandBuffer.

4. **`GERenderTarget::RenderPassDesc` vs `GERenderPassDescriptor`.**
   These two structs duplicate each other today
   ([GERenderTarget.h:22–67](../include/omegaGTE/GERenderTarget.h),
   [GECommandQueue.h:59–73](../include/omegaGTE/GECommandQueue.h)) — the
   wrapper's job is to translate one to the other
   ([GERenderTarget.cpp:37–48](../src/GERenderTarget.cpp)). After the
   wrapper goes away there's only `GERenderPassDescriptor`. **Recommendation:
   delete `GERenderTarget::RenderPassDesc` outright; keep
   `GERenderPassDescriptor`**

   YES

## Migration cost estimate

| Area | Files touched | Risk |
|---|---|---|
| Public headers | 3 | low — straightforward |
| GTE engine cores (Metal/D3D12/Vulkan) | ~10 | medium — RT/queue lifecycle shifts |
| WTK compositor | ~6 | medium — every frame path uses this |
| Tests | 4 | low |
| TEContext refactor | 2 | medium — depends on option 1 vs 2 |
| Docs | ~3 | trivial |

Roughly a focused 1–2 day migration if Phase 1 ships cleanly first; the
deprecation window in Phase 1 lets WTK update on its own schedule.

## Non-goals

- Multi-queue scheduling (graphics + compute + copy) — out of scope; this
  refactor unblocks it but does not deliver it.
- Queue-family abstraction on Vulkan — `GECommandQueue` keeps wrapping a
  pool. A future change can split `GraphicsQueue` / `ComputeQueue` /
  `TransferQueue` if needed.
- Per-frame command-buffer pooling beyond what the queue already does.

For these non goals I was thinking of making a GECommandQueueDesc containing
queue type, priority, maxCommandBuffer count. 
