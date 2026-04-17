# Vulkan BlitTest Triangle-Not-Visible — Diagnosis & Multi-Phase Fix Plan

**Target:** `gte/tests/vulkan/BlitTest` — a 2-pass render-to-texture-then-sample test. Pass 1 draws a colored triangle into an offscreen `VK_FORMAT_R8G8B8A8_UNORM` texture, Pass 2 samples that texture onto a fullscreen quad drawn to the swapchain.

**Symptom:** the triangle is not visible on the swapchain. The DIAG-augmented fragment shader in the test returns yellow when `sampled.a <= 0.5` and cyan when all RGB near zero with `a > 0.5`; the true sample is returned only when rgb > 0.01. The magenta clear shows when the quad itself doesn't draw.

**Status:** I have not been able to run the test (macOS host; the test is Linux/GTK/Vulkan). The diagnosis below is derived from static analysis against the Vulkan spec and the canonical Sascha Willems `offscreen` sample. Findings are ordered by likelihood and fixes are phased so each phase ships a testable slice.

---

## 1 · Reference architecture (what working examples do)

The canonical "draw to texture → sample texture" flow in Vulkan, as implemented by Sascha Willems' `offscreen` sample and reproduced by most Vulkan renderers that do post-processing:

1. Offscreen image is created with `VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT`.
2. Offscreen render pass declares the attachment with `initialLayout = VK_IMAGE_LAYOUT_UNDEFINED`, `finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL` — the render pass performs the transition automatically on `vkCmdEndRenderPass`.
3. The subpass dependency graph names `VK_SUBPASS_EXTERNAL → 0` for the write side (before the pass) with `dstStageMask = COLOR_ATTACHMENT_OUTPUT`, and `0 → VK_SUBPASS_EXTERNAL` for the read side (after the pass) with `srcStageMask = COLOR_ATTACHMENT_OUTPUT`, `srcAccessMask = COLOR_ATTACHMENT_WRITE`, `dstStageMask = FRAGMENT_SHADER`, `dstAccessMask = SHADER_READ`. This makes the write→sample handoff explicit inside the render pass without requiring a separate `vkCmdPipelineBarrier`.
4. The descriptor for the sampled texture is written with `imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL` — the layout the image is in when the shader reads it.
5. The sampler comes from an explicit `VkSampler` (or an immutable sampler baked into the descriptor set layout). Separate samplers + textures use `texture(sampler2D(tex, samp), uv)` in GLSL for Vulkan, which maps to a SPIR-V `OpSampledImage` combining a `OpTypeImage` + `OpTypeSampler`.
6. Work for pass 1 and pass 2 is typically submitted on the same `VkQueue` with a semaphore or a pipeline barrier between the two command buffers. `vkDeviceWaitIdle` between passes is valid (if heavy-handed) and eliminates any ambiguity about whether pass 1's writes are visible.

---

## 2 · What OmegaGTE actually does, cross-referenced against the above

### 2.1 Image usage flags (OK)

[`gte/src/vulkan/GEVulkan.cpp:1016-1024`](../../src/vulkan/GEVulkan.cpp#L1016) — `GETexture::RenderTarget` maps to `VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT`. Offscreen image can be a color attachment and then a sampled image. **No bug here.**

### 2.2 Offscreen render-pass final layout (bug candidate #1)

[`gte/src/vulkan/GEVulkanCommandQueue.cpp:487-544`](../../src/vulkan/GEVulkanCommandQueue.cpp#L487) — texture-target render pass is built with:
- `attachmentDescription.initialLayout = textureTarget->texture->layout` (UNDEFINED on first use)
- `attachmentDescription.finalLayout = VK_IMAGE_LAYOUT_GENERAL`
- one subpass dependency `VK_SUBPASS_EXTERNAL → 0` covering the `COLOR_ATTACHMENT_OUTPUT → COLOR_ATTACHMENT_OUTPUT` write-side edge
- **no `0 → VK_SUBPASS_EXTERNAL` dependency** for the subsequent sample — the read-side edge is left to an explicit pipeline barrier recorded later by `insertResourceBarrierIfNeeded`.

This is legal but fragile. `VK_IMAGE_LAYOUT_GENERAL` is unusual; the reference design uses `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL` so the render pass transitions the image to a read-ready state on its own and no external barrier is strictly required. Choosing GENERAL pushes the correctness burden onto the barrier that `bindResourceAtFragmentShader` emits.

### 2.3 Barrier that transitions GENERAL → SHADER_READ_ONLY_OPTIMAL (bug candidate #2)

[`gte/src/vulkan/GEVulkanCommandQueue.cpp:165-230`](../../src/vulkan/GEVulkanCommandQueue.cpp#L165) — the barrier's `oldLayout` is read from `texture->layout` (GENERAL, set at line 532 when the render pass was recorded) and `newLayout` is `SHADER_READ_ONLY_OPTIMAL`. The barrier trigger is `(priorShaderAccess2 != 0 && hasPipelineAccess) || texture->layout != layout` — the `||` disjunct fires because GENERAL ≠ SHADER_READ_ONLY. Good.

But: `priorShaderAccess2` and `priorPipelineAccess2` are NEVER set by `startRenderPass` when the texture was just used as a render-pass attachment. They get set only by shader bind paths. So the barrier's `srcAccessMask` falls back to `VK_ACCESS_2_MEMORY_WRITE_BIT_KHR` and `srcStageMask` falls back to `VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT_KHR` (lines 204-215).

This "works" only because `vkDeviceWaitIdle` is called between the two passes — the implicit full-device flush makes the previous color-attachment writes globally visible so any permissive barrier succeeds. Remove the `waitForGPUIdle` and the test would likely stop working on a strict driver or validation layer. Even with the wait, the validation layer may (spec-wise correctly) flag the barrier as using a wrong access mask for the previous operation, and some drivers are known to optimize unpredictably when `srcAccessMask` is over-broad.

### 2.4 Descriptor write `imgInfo.imageLayout` (OK, but dependent on 2.3)

[`gte/src/vulkan/GEVulkanCommandQueue.cpp:689-747`](../../src/vulkan/GEVulkanCommandQueue.cpp#L689) — the barrier is inserted first (line 704), which updates `texture->layout` to `SHADER_READ_ONLY_OPTIMAL`. Then `imgInfo.imageLayout = vk_texture->layout` (line 715) uses that updated value. At draw time the image IS in `SHADER_READ_ONLY_OPTIMAL` (assuming 2.3 ran correctly) and the descriptor matches. **Not an independent bug; it's only correct because 2.3 happens to be correct.**

### 2.5 OmegaSL shader codegen (OK)

[`gte/omegasl/src/GLSLCodeGen.cpp:521-601`](../../omegasl/src/GLSLCodeGen.cpp#L521) — `texture2d` emits `uniform texture2D`, `static sampler2d` emits `uniform sampler`, and `sample(sampler, tex, uv)` emits `texture(sampler2D(tex, samp), uv)` (line 855). This is the canonical Vulkan GLSL pattern for separate textures + samplers and produces correct SPIR-V. **No bug here.**

### 2.6 Immutable sampler baked into descriptor set layout (OK)

[`gte/src/vulkan/GEVulkan.cpp:1226-1275`](../../src/vulkan/GEVulkan.cpp#L1226) — `OMEGASL_SHADER_STATIC_SAMPLER2D_DESC` creates a `VkSampler` and assigns it via `binding.pImmutableSamplers`. The pre-reserve on line 1192 protects against vector reallocation invalidating the pointer. Static-sampler descriptor sets are allocated from a pool (lines 1347-1370) even when push descriptors are enabled for set 0 — this is the `setPushDescriptor + static sampler set` split. **Correctly implemented.**

### 2.7 Descriptor set binding indices (OK)

[`gte/omegasl/src/GLSLCodeGen.cpp:480-600`](../../omegasl/src/GLSLCodeGen.cpp#L480) — vertex shader uses `set=0`, fragment uses `set=1`, both with `binding` starting at 0 and incrementing per resource. `layoutDesc.location = registerNumber` (the user-facing ID), `layoutDesc.gpu_relative_loc = binding` (the actual Vulkan binding). [`gte/src/vulkan/GEVulkanCommandQueue.cpp:36-44`](../../src/vulkan/GEVulkanCommandQueue.cpp#L36) resolves `bindResourceAtFragmentShader(tex, 2)` → binding 0, which matches the descriptor set layout. **No bug here.**

### 2.8 Pipeline compatibility render pass format (OK)

Pipeline colorPipeline is created with `colorPixelFormat = RGBA8Unorm` → `VK_FORMAT_R8G8B8A8_UNORM`, matching the offscreen texture format. copyPipeline uses `nativeTarget->pixelFormat()`, which round-trips through [`gte/src/vulkan/GEVulkanRenderTarget.cpp:91-100`](../../src/vulkan/GEVulkanRenderTarget.cpp#L91) — swapchain format (e.g. `B8G8R8A8_UNORM`) is preserved. **Compatible per Vulkan 8.2 render pass compatibility rules.**

### 2.9 Blend state (bug candidate #3 — low likelihood for this specific test, but a design smell)

[`gte/src/vulkan/GEVulkan.cpp:1629-1640`](../../src/vulkan/GEVulkan.cpp#L1629) — `colorBlendAttachment.blendEnable = VK_TRUE` with `SRC_ALPHA / ONE_MINUS_SRC_ALPHA`, hard-coded for every graphics pipeline. With the DIAG shader returning alpha=1 the blit blends to solid output so this does not cause the invisible triangle. But this same pipeline policy will bite any test that samples a texture whose alpha channel happens to be zero — such a texture would blend to zero contribution on the swapchain and produce the magenta clear.

### 2.10 Two separate `GEVulkanCommandQueue` instances (bug candidate #4 — sync concern, mitigated by waitForGPUIdle)

[`gte/src/vulkan/GEVulkanRenderTarget.cpp:60`](../../src/vulkan/GEVulkanRenderTarget.cpp#L60) and [`:227`](../../src/vulkan/GEVulkanRenderTarget.cpp#L227) — each render target owns its own `GEVulkanCommandQueue`, created by `parentEngine->makeCommandQueue(100)`. Pass 1 submits on the texture-target queue. Pass 2 submits on the native-target queue. If both queues are backed by the same `VkQueue` from the same family, there is no queue ownership transfer needed; `vkDeviceWaitIdle` makes pass 1's writes globally visible. If a driver ever picks different queue families for the two `makeCommandQueue` calls the image-sharing-mode becomes relevant — `queueFamilyIndices.size() > 1` causes `VK_SHARING_MODE_CONCURRENT` at [`GEVulkan.cpp:1044-1047`](../../src/vulkan/GEVulkan.cpp#L1044), which is the correct defensive behavior. **Not the likely cause, but worth hardening.**

### 2.11 Fence semantics (minor, not the cause)

`textureTarget->submitCommandBuffer(cb, fence)` uses `vkCmdSetEvent` on a `VkEvent` stored in `GEVulkanFence` ([`GEVulkanCommandQueue.cpp:1394-1396`](../../src/vulkan/GEVulkanCommandQueue.cpp#L1394)). A `VkEvent` is device-side and cannot be waited on by the host via `vkWaitForFences`. This is why the test added `waitForGPUIdle()` — the `fence` object alone does not synchronize with the host. Naming is misleading. **Not the cause; the explicit idle already compensates.**

---

## 3 · Likely root cause, summarized

I did not find a single unambiguous bug. The test reaches Pass 2's fragment shader, the descriptor is correctly set up, the layout transition is recorded, and `vkDeviceWaitIdle` makes pass 1's writes visible. Based on the DIAG shader structure in the test, the most probable runtime observations are:

- **cyan across the quad** (sample returns rgb=0, alpha=1) — this would indicate the VkImageView the descriptor points at is valid but its contents are all zero. Possible causes: Pass 1's render pass didn't actually clear or draw (startRenderPass returned early for some reason — there are `return;` exits on vkCreateRenderPass / vkCreateFramebuffer failure that go silent); the pipeline's compatibility render pass format mismatches the attachment at draw time (drivers may discard draws); or the `oldLayout = UNDEFINED` on first Pass 1 → GPU discards the contents of the image when no loadOp=CLEAR is done (it is, so this should be fine).
- **yellow across the quad** (alpha <= 0.5) — would indicate the shader is sampling from a *different* image than the offscreen texture, or the descriptor was never actually populated (bind-time warning in the DIAG output would say so).
- **magenta** (quad did not draw) — pipeline incompatibility between pipeline's compatibility render pass and the swapchain render pass, or pipeline state issue (cull mode, topology, viewport).

The existing `[DIAG bindFragTex]` and `[DIAG startRP-tex]` stderr prints in the backend will disambiguate between these. **The next step should be to run the test once and capture those log lines** — they will point at the specific failure immediately, and the fixes below should be applied in the order the evidence justifies.

In the absence of that log, the highest-leverage structural fix is **Phase 1** below: move the offscreen render pass to the canonical `finalLayout = SHADER_READ_ONLY_OPTIMAL` + proper subpass dependency graph. That fix is correct by construction and eliminates the largest class of synchronization bugs in one change. If the triangle is still invisible after Phase 1, the `[DIAG]` logs will be the authoritative signal for the next phase.

---

## 4 · Multi-phase fix plan

Each phase stands alone, is testable, and does not regress callers outside BlitTest. Land them in order; re-run BlitTest between phases.

### Phase 1 — Render-pass-managed layout transition (high priority, low risk)

**Goal:** eliminate the explicit barrier and layout-tracking fragility by letting the offscreen render pass transition the image to `SHADER_READ_ONLY_OPTIMAL` itself, matching the canonical design.

**Files:**
- `gte/src/vulkan/GEVulkanCommandQueue.cpp`

**Changes in `startRenderPass` (texture-target branch, ~L487-544):**
- Set `attachmentDescription.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL` when the texture's usage is `RenderTarget` and the texture is intended for subsequent sampling. (For render targets that will be re-rendered the next frame, keep `GENERAL`; gate on usage flag or an explicit bit in `TextureRenderTargetDescriptor`.)
- Add a second `VkSubpassDependency` `0 → VK_SUBPASS_EXTERNAL` with `srcStageMask = COLOR_ATTACHMENT_OUTPUT`, `srcAccessMask = COLOR_ATTACHMENT_WRITE`, `dstStageMask = FRAGMENT_SHADER`, `dstAccessMask = SHADER_READ`, `dependencyFlags = BY_REGION`.
- Update `textureTarget->texture->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL` after the render pass (at line 532, replacing GENERAL).
- Update `priorPipelineAccess2 = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT_KHR` and `priorShaderAccess2 = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT_KHR` to reflect the attachment-write that just happened. This makes any future barrier correct.

**Changes in `insertResourceBarrierIfNeeded` (texture, ~L165-230):**
- Now that the render pass did the transition, `texture->layout == SHADER_READ_ONLY_OPTIMAL` before Pass 2's bind, and the `||` disjunct at line 201 is false — no barrier is emitted. Good.
- If `priorPipelineAccess2 == COLOR_ATTACHMENT_OUTPUT`, the `srcAccessMask` path (line 204) correctly names `COLOR_ATTACHMENT_WRITE_BIT` via the fallback, so in the rare case the barrier does fire it is now accurate.

**Success criteria:** BlitTest renders the triangle. Validation layer is silent about layout mismatches.

**Risk:** other tests that re-render the offscreen target between samples. Mitigation: gate the `SHADER_READ_ONLY_OPTIMAL` final layout on a usage hint; default to GENERAL otherwise (keeps current behavior for MSResolveSrc and others). Small additive change.

---

### Phase 2 — Remove `vkDeviceWaitIdle` from BlitTest (high priority, low risk, *gated on Phase 1*)

**Goal:** prove the synchronization is correct without the sledgehammer.

**Files:**
- `gte/tests/vulkan/BlitTest/main.cpp`

**Change:** delete the `gte.graphicsEngine->waitForGPUIdle()` call between the two passes. With Phase 1's subpass dependency and `SHADER_READ_ONLY_OPTIMAL` final layout, the cross-pass hazard is closed at the render-pass level and the explicit idle is unnecessary.

**Success criteria:** triangle still visible, validation layer silent. If either fails, the idle goes back in and we fall through to Phase 3.

---

### Phase 3 — Unify command queues or use semaphores (medium priority, higher risk)

**Goal:** replace the ad-hoc two-queue model with explicit semaphore-based handoff, so Pass 1 → Pass 2 is a proper GPU-side dependency.

**Files:**
- `gte/src/vulkan/GEVulkanCommandQueue.cpp`
- `gte/src/vulkan/GEVulkanCommandQueue.h`
- `gte/src/vulkan/GEVulkanRenderTarget.cpp`
- `gte/src/GERenderTarget.cpp` (common-layer fence API)

**Changes:**
- Replace `GEVulkanFence::event` (`VkEvent`) with a pair: a `VkSemaphore` for GPU→GPU handoff and a `VkFence` for GPU→host waits. Update `notifyCommandBuffer` to add the semaphore to the next submission's `pWaitSemaphores`. Update `submitCommandBuffer(cb, fence)` to signal the semaphore (and the host-side fence, so `waitForFence` becomes valid).
- Callers like BlitTest can then use `notifyCommandBuffer(cb2, fence)` → `submitCommandBuffer(cb1, fence)` to chain Pass 1 → Pass 2 without touching `waitForGPUIdle`.

**Risk:** this is invasive — every caller of `makeFence`/`submitCommandBuffer(cb, fence)` needs review. Consider doing this in a separate PR after Phase 1 and 2 prove the layout-transition theory was right.

---

### Phase 4 — Remove forced blend in pipeline creation (low priority, low risk)

**Goal:** stop hardcoding `blendEnable = VK_TRUE`; take it from `RenderPipelineDescriptor` or default off.

**Files:**
- `gte/src/vulkan/GEVulkan.cpp:1629-1640`

**Change:** default `blendEnable = VK_FALSE`; add a field to `RenderPipelineDescriptor` (common) and plumb it through Metal/D3D12/Vulkan. BlitTest doesn't need blending for this test but any future test sampling a texture with non-unit alpha will lose pixels silently with the current default.

**Success criteria:** no visual change for BlitTest, but the API now honors the descriptor.

---

### Phase 5 — Improve startRenderPass failure visibility (low priority, very low risk)

**Goal:** make the silent `return;` paths in `startRenderPass` (lines 419-481) loud enough that "triangle not visible" becomes traceable in stderr.

**Files:**
- `gte/src/vulkan/GEVulkanCommandQueue.cpp`

**Change:** emit `std::cerr` at every early return, including `[DIAG startRP-native]` mirror of the `[DIAG startRP-tex]` block. Optional but cheap; pays for itself the next time a test silently fails.

---

## 5 · What to do next, concretely

1. **Run BlitTest once and capture stderr.** The existing `[DIAG]` lines will reveal whether descriptors are being written, what layout the texture is in, and whether render passes are being created.
2. Based on that output:
   - If `[DIAG bindFragTex] descriptor written OK` appears with the correct binding and layout → apply Phase 1 immediately. This is the canonical design and is correct by construction.
   - If `[DIAG bindFragTex] WARNING: descs empty` appears → the fragment descriptor set was never allocated; the bug is in the push-descriptor / non-push descriptor allocation split at `GEVulkan.cpp:1347-1370`, probably specific to pipelines with static samplers. Separate fix path.
   - If the offscreen `[DIAG startRP-tex]` line never appears → the texture render pass is not being recorded (creation failure). Apply Phase 5 first to get a diagnostic.
3. Apply Phase 1, rerun, confirm, commit.
4. Apply Phase 2, rerun, confirm, commit.
5. Leave Phase 3–5 for follow-up PRs unless BlitTest is still broken.

---

## 6 · References

- Vulkan spec 8.2 — Render Pass Compatibility
- Vulkan spec 7.4 — Image Layout Transitions
- Sascha Willems samples: `offscreen` (https://github.com/SaschaWillems/Vulkan/tree/master/examples/offscreen) — the canonical render-to-texture-and-sample reference
- Vulkan Guide chapter on subpass dependencies (https://vkguide.dev/docs/extra-chapter/implementing_push_constants/)
