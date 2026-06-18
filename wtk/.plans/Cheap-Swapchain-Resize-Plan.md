# Cheap Swapchain Resize Plan

**Status:** proposal (2026-06-17). Supersedes **Phase F-G** ("Bucketed
render-target + image allocation") of
[`wtk/.plans/UIView-Render-Redesign-Plan.md`](../../wtk/.plans/UIView-Render-Redesign-Plan.md).

**Author note on doc location.** This plan lives under `gte/.plans/` at the
developer's request even though the new mechanism is implemented in WTK — the
*other half* of the work is the removal of GTE-side D3D12 code (Phase F-G.1),
which is a GTE change, and the developer chose to keep the whole story in one
GTE-side doc. A supersession pointer is added to the Phase F-G section of the
WTK plan so the two stay cross-referenced.

---

## 1. Why Phase F-G didn't work as well as planned

Phase F (Window resize always relayouts + repaints) made resize *correct*: on
every resize tick the whole widget tree relayouts and re-rasterizes at the new
size, so no content is left stretched. Phase G made that repaint *cheap* with a
content cache. Phase F-G then tried to make the **swap-chain reallocation
itself** cheap, because the one GPU-object recreation that still fires per
resize tick is the swap chain (`resizeSwapChain` →
D3D12 `ResizeBuffers` / Vulkan `vkRecreateSwapchainKHR`, each a full GPU stall).

F-G's answer was **bucketed presentation**:

- **F-G.1 (D3D12, shipped, gated `OMEGAWTK_BUCKETED_SWAPCHAIN`, default OFF).**
  Allocate the back-buffer at a power-of-two bucket ≥ the live size and present
  only the live `[0,0,w,h]` sub-region via `IDXGISwapChain2::SetSourceSize`, so
  in-bucket ticks skip `ResizeBuffers`. See
  `gte/src/d3d12/GED3D12RenderTarget.cpp:165-224` (`resizeSwapChain`,
  `reallocBackBuffers`), the `sourceWidth_/sourceHeight_` fields
  (`gte/src/d3d12/GED3D12RenderTarget.h:67-68`), and the three back-buffer-size
  dependencies rerouted to track the live *source* size: the viewport/scissor
  Y-flip in `GED3D12CommandBuffer::setViewports`/`setScissorRects`, the
  tessellation NDC viewport in `D3D12TEContext.cpp:371-382`
  (`getEffectiveViewport`), and the ctor seed.
- **F-G.2 (Vulkan, never implemented).** Vulkan has no `SetSourceSize` analog;
  the closest is `VK_EXT_swapchain_maintenance1` present scaling, gated on a
  driver capability query that is reportedly patchy, plus an unreliable
  `VK_SUBOPTIMAL_KHR` vs `VK_ERROR_OUT_OF_DATE_KHR` measurement that has to be
  taken per compositor. It stalled at research.

**What went wrong.** The bucketed approach is *complex and backend-divergent*
for the value it returns:

- It is fundamentally per-backend. D3D12 got a `SetSourceSize` path; Vulkan
  needs an entirely different (extension-gated, capability-fragile) mechanism;
  Metal is N/A. Three backends, three stories, two of them unfinished.
- The D3D12 path was never validated to actually look and perform right —
  it is write-only on the macOS dev host and the recent commits
  ("D3D12 bucketed render target size — needs to be verified",
  "WTK:D3D12 fixes") reflect it still settling, not landed-and-trusted.
- It keeps *paying per tick* — it makes the swap-chain cost cheaper, but the
  tree still relayouts and re-rasterizes every single resize tick (Phase F's
  `forceFullRepaint`). The bucket only removes one of several per-tick costs.
- It carries subtle coordinate-space hazards (the D3D12 top-left/bottom-left
  Y-flip bug found at line 2594-2609 of the WTK plan was a direct consequence
  of the source-size split).

The developer's judgment: this is the wrong altitude. We are fighting the OS to
keep the window crisp *during* a drag, when the OS already does the obvious
cheap thing for us — it **stretches the last-presented surface** to the live
window size while a resize is in flight. That is exactly what every other
native app's window looks like mid-drag. We should lean into it instead of
out-engineering it.

---

## 2. The new idea — Cheap Swapchain Resize

**Suspend content drawing for the duration of an interactive resize, and let
the native surface stretch. Do exactly one relayout + repaint + swap-chain
resize when the drag finishes.**

During an OS-driven window-edge drag:

1. WTK **stops producing frames** — no `forceFullRepaint`, no `buildFrame`, no
   present.
2. WTK **stops resizing the swap chain** — no `syncNativePresentLayer`, so no
   `resizeSwapChain` / `ResizeBuffers` / `vkRecreateSwapchainKHR` per tick.
3. The OS compositor (DWM on Windows, the X/Wayland compositor on Linux,
   CoreAnimation on macOS) **stretches the last-presented surface** to fill the
   live window rect. Mid-drag content is bilinear-stretched — slightly soft,
   exactly like a normal native window resize. This is the accepted fidelity
   trade (the same trade G.5.4's drag-stretch already chose, but achieved for
   free by the OS rather than by a GPU blit).

When the drag ends (`WindowHasFinishedResize`):

4. WTK runs **one** `syncNativePresentLayer(finalRect)` → one swap-chain resize
   to the final size.
5. WTK runs **one** `notifyWindowResizeEnd(finalRect)` → one `handleHostResize`
   (relayout) + one `forceFullRepaint` → a crisp full-resolution frame at the
   final size.

Net cost of a 500-tick live drag: **zero** per-tick GPU work, **one**
swap-chain recreation, **one** full repaint. The expensive `ResizeBuffers` /
`vkRecreateSwapchainKHR` stall happens once, at rest, where the user is no
longer watching the edge move.

### 2.1 Why this is uniform across all backends

The whole mechanism lives **above** the backend. "Suspend drawing" means WTK
simply does not issue a `buildFrame`; "don't resize the swap chain" means WTK
does not call `syncNativePresentLayer`. Neither requires any backend to present
a sub-rect, hold a stale extent, or query a present-scaling capability. The
backend does nothing special — it is just not asked to do anything until the
drag ends. The OS surface-stretch is standard window-manager behavior on all
three platforms. That is why the option is a **WTK** CMake/env toggle and is
"available everywhere" with no per-backend code.

This is the key advantage over F-G: F-G needed three different backend
mechanisms (one shipped, one unfinished, one N/A). Cheap Swapchain Resize needs
**zero** backend mechanisms.

### 2.2 The elegant part — the drag-end path already exists

The drag-end path already does exactly the right thing today:

- `AppWindowDelegate::dispatchResizeEndToHosts` (`wtk/src/UI/AppWindow.cpp:627`)
  already calls `syncNativePresentLayer(rect)` (one swap-chain resize) **and**
  `WidgetTreeHost::notifyWindowResizeEnd(rect)`.
- `WidgetTreeHost::notifyWindowResizeEnd` (`wtk/src/UI/WidgetTreeHost.cpp:619`)
  already calls `root->handleHostResize(rect)` + `forceFullRepaint()` (one
  relayout + one crisp repaint) and clears `resizing_`.

So Cheap Swapchain Resize is **almost entirely a suppression** of the
*begin* and *tick* work. We do not build new drag-end machinery — we let the
existing end path produce the single crisp frame and simply stop doing the
per-tick work that precedes it.

---

## 3. Mechanism (WTK)

### 3.1 The toggle

Mirror the two existing patterns in the tree (the content-cache CMake option
and the bucketed-swapchain env override):

- **CMake option** `OMEGAWTK_ENABLE_CHEAP_SWAPCHAIN_RESIZE` (default OFF; see
  §6 Open question 1) in `wtk/CMakeLists.txt`, next to
  `OMEGAWTK_ENABLE_CONTENT_CACHE` (`wtk/CMakeLists.txt:14`). When ON, define
  `OMEGAWTK_CHEAP_SWAPCHAIN_RESIZE_ENABLED=1` on the `OmegaWTK_UI` target
  (both consumers — `AppWindow.cpp` and `WidgetTreeHost.cpp` — live there).
- **Env override** `OMEGAWTK_CHEAP_SWAPCHAIN_RESIZE` (`1`/`on`/`true` vs
  `0`/`off`/`false`) so a single binary can A/B without rebuilding, exactly as
  `bucketedSwapChainEnabled()` does at
  `gte/src/d3d12/GED3D12RenderTarget.cpp:38-52`.
- A small read-once helper `cheapSwapchainResizeEnabled()` in the `OmegaWTK_UI`
  module (anonymous-namespace free function, build-default + env override,
  cached for process lifetime — same shape as `bucketedSwapChainEnabled()` /
  `ContentCacheConfig::inst()`).

### 3.2 The suppression — only during an interactive drag

The gate is **not** simply "cheap-resize enabled." It is "cheap-resize enabled
**and** we are inside an OS-driven interactive drag." A one-shot programmatic
resize (`AppWindow::setRect`) or a DPI change (`WindowScaleFactorChanged` →
`dispatchResizeToHosts`) has no "drag end" to trigger the crisp repaint, so it
must still do the full work. The existing interactive-drag bracket is the right
signal:

- `AppWindowDelegate::liveResizeActive` (`wtk/include/omegaWTK/UI/AppWindow.h:301`)
  — true between the first `WindowWillStartResize`/`WindowWillResize` and the
  `WindowHasFinishedResize` (`AppWindow.cpp:659-662`, `:790`).
- `WidgetTreeHost::isResizing()` / `resizing_`
  (`WidgetTreeHost.h:259-260`, set in `notifyWindowResizeBegin`, cleared in
  `notifyWindowResizeEnd`) — the WidgetTreeHost-side mirror of the same drag.

The six guarded sites (all in `OmegaWTK_UI`):

| Site | File:line | Change when `cheap-resize` is on |
| --- | --- | --- |
| `dispatchResizeBeginToHosts` | `AppWindow.cpp:618` | skip `syncNativePresentLayer(rect)` (a begin is always a drag start) |
| `dispatchResizeToHosts` | `AppWindow.cpp:607` | skip `syncNativePresentLayer(rect)` **only when `liveResizeActive`** (so DPI-change / programmatic resize still resizes the swap chain) |
| `dispatchResizeEndToHosts` | `AppWindow.cpp:627` | **unchanged** — always does the one resize |
| `notifyWindowResizeBegin` | `WidgetTreeHost.cpp:592` | still set `resizing_ = true`; skip `handleHostResize` + `forceFullRepaint` |
| `notifyWindowResize` | `WidgetTreeHost.cpp:568` | skip `handleHostResize` + `forceFullRepaint` **only when `resizing_`** |
| `notifyWindowResizeEnd` | `WidgetTreeHost.cpp:619` | **unchanged** — always does the one relayout + repaint, clears `resizing_` |

`window->impl_->rect = rect` is set at the top of each `dispatchResize*ToHosts`
*before* the suppressed `syncNativePresentLayer` call, so the latest live rect
is always recorded and drag-end reads the correct final size with no extra
plumbing. The diagnostic `OMEGAWTK_DEBUG`/resize-tracker bookkeeping in the
`notify*` methods can stay (it is cheap and useful for the validator); only the
`handleHostResize` + `forceFullRepaint` pair is gated.

### 3.3 Why both halves must be suppressed together

Suppressing only the repaint but still resizing the swap chain per tick would
recreate the swap chain at the new size with **no content drawn into it** → a
blank/garbage flash mid-drag. Suppressing only the swap-chain resize but still
repainting would rasterize the tree against a stale backing extent. Both the
`syncNativePresentLayer` resize **and** the `forceFullRepaint` must be held off
together for the OS-stretch to be the only thing the user sees. The end path
re-couples them in the correct order (resize first, then repaint) exactly as it
does today.

### 3.4 Estimated scope

~50–80 LOC in WTK: the `cheapSwapchainResizeEnabled()` helper, the CMake option
+ define, and the six guarded call sites. No new types, no backend change, no
`DrawOp` change.

---

## 4. Mechanism (GTE) — remove Phase F-G.1

With Cheap Swapchain Resize as the resize path, `resizeSwapChain` fires **once
per drag** (at the end), not per tick. The entire reason F-G.1 existed — to make
the *per-tick* `ResizeBuffers` cheap — is gone. The bucketed back-buffer +
`SetSourceSize` machinery is now pure complexity with no remaining caller-side
benefit, so it is removed:

**Deleted from `gte/src/d3d12/GED3D12RenderTarget.cpp`:**
- `bucketedSwapChainEnabled()`, `bucketDim()`, `nextPow2()` (anonymous
  namespace, lines 9-53) — unless `bucketDim`/`nextPow2` are used elsewhere in
  the file (grep before deleting; they are local to this TU today).
- The `if (bucketedSwapChainEnabled())` bucketed branch of `resizeSwapChain`
  (lines 173-212) — the `SetSourceSize` + grow-to-bucket logic.
- `resizeSwapChain` reverts to the plain exact-size path (current lines
  214-223): skip if the buffer already matches, else `reallocBackBuffers(width,
  height)`. This is the pre-F-G.1 behavior.

**Deleted from `gte/src/d3d12/GED3D12RenderTarget.h`:**
- `sourceWidth_` / `sourceHeight_` (lines 67-68) and their doc comment
  (lines 56-66) — and the ctor seed (`GED3D12RenderTarget.cpp:77-84`).

**Reverted source-size dependencies (back to exact buffer size):**
- `D3D12TEContext.cpp:371-382` (`getEffectiveViewport`) — read the back-buffer
  desc width/height directly again instead of `sourceWidth_/sourceHeight_`.
- The viewport/scissor Y-flip in `GED3D12CommandBuffer::setViewports` /
  `setScissorRects` — confirm it reads the buffer size, not a tracked source
  size. (Note: the Y-flip itself was *separately* fixed to top-left at WTK plan
  line 2594-2609; that top-left fix is correct and **stays** — only the
  source-size *reference* reverts. Verify the two changes are disentangled
  before editing.)

**Deleted from `gte/CMakeLists.txt`:**
- The `OMEGAGTE_ENABLE_BUCKETED_RENDER_TARGET` option + the
  `OMEGAGTE_BUCKETED_RENDER_TARGET_ENABLED=1` define (lines 155-166).

**Vulkan:** F-G.2 was never implemented, so there is nothing to remove from
`GEVulkanRenderTarget` beyond ensuring `resizeSwapChain` stays the plain
recreate path (it already is). The F-G.2 research block in the WTK plan becomes
historical.

> **OFF-PLATFORM — UNVERIFIED FROM THIS HOST.** All of §4 is D3D12 code that is
> write-only on the macOS/Linux dev hosts (D3D12 needs the Windows SDK). The
> removal must be compiled and resize-verified on a Windows build by the
> developer before it is trusted — same constraint that made F-G.1 itself
> write-only. Hand the build off per AGENTS.md "Windows builds go through WSL."

### 4.1 Trade-off of removal (honest)

Removing F-G.1 means that **with cheap-resize OFF**, the D3D12 backend reverts
to exact-size `ResizeBuffers` per tick — the slow, pre-F-G.1 behavior. That is
acceptable because:

- Cheap-resize is intended to be the answer; OFF is a debug/fallback A/B switch,
  not the production path.
- F-G.1 was never validated as a win anyway, so we are not giving up a proven
  optimization — we are removing an unproven, half-finished one.

If the developer wants to keep a per-tick swap-chain optimization for the
cheap-resize-OFF case, F-G.1 could instead be left in place behind its existing
(default-OFF) flag rather than deleted — see §6 Open question 2.

---

## 5. Phasing

A small, two-phase change (each well under the ~300-LOC "small feature"
threshold, so no deep sub-phase breakdown):

**Phase 1 — WTK suspend mechanism [~50–80 LOC, this host].**
The `cheapSwapchainResizeEnabled()` helper, the CMake option + define, and the
six guarded sites in §3.2. Verifiable on the native Vulkan target on this Linux
host: a window-edge drag of `BasicAppTest` should show the content stretch
(soft) during the drag and snap crisp on release, with no per-tick relayout
spam in the resize-tracker `OMEGAWTK_DEBUG` log. Ships independently — turning
the option on does nothing until Phase 1 lands, and Phase 1 is correct with or
without Phase 2 (F-G.1 ON or removed).

**Phase 2 — GTE F-G.1 removal [~130 LOC deleted, Windows-only verify].**
The §4 deletions. Depends on nothing in Phase 1 mechanically (it is a pure
revert), but is *motivated* by Phase 1 — sequence it after Phase 1 is accepted
so we never have a window where the swap chain is bucketed but never per-tick
resized. Must be Windows-built + resize-verified by the developer.

---

## 6. Open questions / decisions for the developer

1. **Default ON or OFF for `OMEGAWTK_ENABLE_CHEAP_SWAPCHAIN_RESIZE`?**
   *Recommendation: OFF (opt-in) initially*, matching the codebase convention
   for resize/fidelity features that change visible behavior before they are
   visually validated (`OMEGAWTK_ENABLE_CONTENT_CACHE` OFF, F-G's
   `OMEGAGTE_ENABLE_BUCKETED_RENDER_TARGET` OFF, G.5.4's `OMEGAWTK_RESIZE_STRETCH`
   OFF). Flip to default ON once the mid-drag stretch + crisp-snap is confirmed
   good on all three platforms. The developer may prefer ON immediately since
   this is meant to be *the* resize path "available everywhere."

2. **Delete F-G.1 outright, or leave it gated-off?** The developer's stated
   intent ("the D3D12 code in GTE for this won't be necessary anymore") points
   to deletion (§4). The alternative is to leave F-G.1 behind its existing
   default-OFF flag so the cheap-resize-OFF path still has a per-tick swap-chain
   optimization. Deletion is cleaner and removes a coordinate-space hazard
   (§4); keeping it preserves an A/B option. Recommendation: delete, per the
   stated intent.

3. **Concurrent animation during a resize drag.** If a property/element
   animation is ticking while the user drags the window edge, suppressing all
   painting freezes that animation for the drag's duration; it catches up at
   drag-end. Resize drags are brief and user-driven, so a recommended default
   is *suspend everything* (simplest, uniform). The alternative — keep painting
   animated views while the window stretches — defeats the "zero per-tick GPU
   work" goal and reintroduces the swap-chain-resize question. Recommendation:
   suspend; revisit only if a real scene shows an objectionable hitch.

4. **Per-backend OS-stretch behavior is general-pattern, not yet verified
   here.** "The OS stretches the last-presented surface while drawing is
   suspended" is standard window-manager behavior, but the exact result differs
   per platform (DWM flip-model + `DXGI_SCALING_STRETCH` on D3D12; X/Wayland
   compositor scaling for Vulkan; `CAMetalLayer` content stretch on Metal) and
   has not been observed from this host on any backend yet. Each needs a visual
   drag test. The Vulkan resume-at-end path will take one `OUT_OF_DATE` →
   recreate on the first post-drag acquire — that is the single intended
   recreation, already handled by `resizeSwapChain` at drag-end.

5. **Option naming.** `OMEGAWTK_ENABLE_CHEAP_SWAPCHAIN_RESIZE` (CMake) /
   `OMEGAWTK_CHEAP_SWAPCHAIN_RESIZE` (env). Open to a shorter name if the
   developer prefers.

---

## 7. Validators

- **This host (Vulkan, Phase 1):** window-edge drag of `BasicAppTest` from
  800×600 → 1600×900 with the option ON. Expect: content visibly stretches
  (soft) through the drag, exactly one crisp full-resolution repaint on release,
  and the resize-tracker `OMEGAWTK_DEBUG` log showing **no** per-tick
  `ResizeSession` repaint storm — only the begin marker, then the end repaint.
  With the option OFF, the current per-tick relayout+repaint behavior is
  unchanged (regression guard).
- **Windows (D3D12, Phase 2):** after the §4 removal, a Windows build links and
  runs; a window drag produces one `ResizeBuffers` at drag-end (not per tick),
  crisp final frame, no `[F-G]` `SetSourceSize` traces (they are deleted), and
  no Y-flip / off-texture regression (the top-left convention fix is preserved).
- **Headless:** the programmatic clamp/resize tests (e.g.
  `ContainerClampAnimationTest`, `LayoutResizeStressTest`) must be **unaffected**
  — they drive `setRect`/programmatic resize, not an OS drag, so `liveResizeActive`
  / `resizing_` stay false and the full relayout+repaint runs as today. This is
  the §3.2 "only during an interactive drag" guard doing its job.

---

## 8. Relationship to existing plans

- **Supersedes** Phase F-G of `wtk/.plans/UIView-Render-Redesign-Plan.md`
  (both F-G.1 D3D12 and the F-G.2 Vulkan research). A supersession pointer is
  added to that section.
- **Builds on** Phase F (the begin/tick/end resize event machinery and
  `forceFullRepaint`) and the G.5.4 `WidgetTreeHost::isResizing()` /
  `liveResizeActive` drag signal — this plan reuses those signals to decide
  *when* to suspend, rather than adding new ones.
- **Makes G.5.4's drag-stretch (`OMEGAWTK_RESIZE_STRETCH`) redundant during a
  drag** — the OS does the stretch for free once painting is suspended, so the
  G.3 content-cache blit-stretch path no longer runs mid-drag. G.5.4 can stay
  as an independent opt-in (it is already default OFF) or be retired in a later
  cleanup; not in scope here.
- **Simplifies Phase H** (frame pacing) — there are no per-tick resize
  reallocation latency spikes left for the pacer to reason about, which Phase H
  explicitly wanted removed first (WTK plan line 3913-3916).
