# Render Execution Efficiency Plan

Address the architectural flaws in the compositor that cause frame
rendering to take seconds, not milliseconds, during resize. The problem
exists at three levels:

1. **Native view tree.** ~~OmegaWTK creates one native platform view
   per widget.~~ **NV-1, NV-2, and NV-3 are complete:** child widgets
   are virtual (no native views), input events route through hit testing,
   and all widgets render into the root view's single backing surface
   via viewport override. One compositor surface, one render target, and
   one GPU texture per window.

2. **Scheduler architecture.** The compositor processes one widget's
   commands at a time, presents after each widget, and only then moves
   to the next. During a resize with N widgets, this produces N
   staggered partial screen updates. The queue fills with hundreds of
   packets that drain long after the resize finishes.

3. **Per-command rendering.** Within each widget's frame, every visual
   command gets its own GPU submission, its own tessellation pass, and
   its own render target rebuild on resize.

All three levels must be fixed. The native view consolidation is the
foundational change — it determines whether the system can ever have one
surface, one render target, and one present per window. The scheduler
architecture determines whether the compositor can produce a coherent
frame from multiple widgets. The per-command rendering determines
whether individual frames can meet the 16.67ms budget.

The Batched Compositing Pass Plan (`Batched-Compositing-Pass-Plan.md`)
addresses the final compositing step (blitting layer textures to the
swapchain). This plan addresses the native view architecture, scheduling
architecture, and per-command rendering that feeds into it.

The Native View Architecture Plan (`Native-View-Architecture-Plan.md`)
provides the full rationale and phased migration for the native view
consolidation. This plan incorporates the rendering-relevant aspects as
Tier 0.

---

## Current execution profile

The following breakdown was measured from the code structure of a
30-visual-command frame during a resize event. Each step is annotated
with its source location and cost category.

| Step | Location | Cost | Category |
|------|----------|------|----------|
| Render target lookup | `Execution.cpp:175` | Low | Map lookup |
| Layer surface target lookup/create | `Execution.cpp:199–232` | **High on resize** | GPU allocation |
| `setRenderTargetSize` → `rebuildBackingTarget` | `RenderTarget.cpp:620–635 → 503–578` | **5–20ms** | Sync GPU wait + allocation |
| Clear | `RenderTarget.cpp:648–662` | Low–Med | Separate GPU submission |
| **Per-command tessellation** (×30) | `RenderTarget.cpp:830–1078` | **15–30ms** | `triangulateSync()` blocks CPU |
| **Per-command vertex generation** (×30) | `RenderTarget.cpp:1164–1254` | 2–5ms | CPU matrix mul per vertex |
| **Per-command GPU submission** (×30) | `RenderTarget.cpp:1266–1297` | **10–20ms** | 30 separate render passes |
| `commit()` | `RenderTarget.cpp:673–722` | **5–15ms** | `waitForGPU()` on effects path |
| Presentation blit | `RenderTarget.cpp:1345–1387` | 2–5ms | One more submission — **eliminated by Phase A-1** |
| **Total** | | **39–95ms → 37–90ms after A-1** | **10–26 FPS during resize** |

The same frame on a production compositor (Chromium, game engine) would
take **under 2ms** — most of it GPU execution time, not CPU overhead.

---

## Root causes

### 1. Per-command GPU submissions

**The single largest problem.** Each visual command in `renderToTarget()`
gets its own complete render pass cycle:

```
cb = preEffectTarget->commandBuffer();
cb->startRenderPass(renderPassDesc);    // Begin render pass
cb->setRenderPipelineState(...);        // Bind pipeline
cb->bindResourceAtVertexShader(...);    // Bind vertex buffer
cb->setViewports({viewport});           // Set viewport
cb->drawPolygons(...);                  // Draw
cb->endRenderPass();                    // End render pass
preEffectTarget->submitCommandBuffer(cb);  // SUBMIT TO GPU
```

This is called once per visual command. A frame with 30 commands produces
30 GPU submissions. Each submission carries fixed overhead: command buffer
allocation, driver validation, GPU context switch, barrier insertion.

**How production systems solve this:**

- **Chromium:** The Skia renderer records all draw operations into a
  single `SkCanvas`, which is backed by a single Skia GPU surface. One
  `GrDirectContext::flush()` at the end submits everything as **one GPU
  command buffer**. Regardless of whether the frame has 5 or 500 draw
  quads, the submission count is 1.

- **Game engines (Vulkan/Metal/DX12):** You record all draw calls into
  a single `VkCommandBuffer` / `MTLRenderCommandEncoder` /
  `ID3D12GraphicsCommandList`, then submit once with
  `vkQueueSubmit()` / `[cb commit]` / `ExecuteCommandLists()`. A
  typical game frame with 200–500 draw calls produces 1–3 GPU
  submissions.

### 2. Per-frame synchronous tessellation

Every visual command is re-tessellated every frame, even if the shape
hasn't changed. The tessellation uses `triangulateSync()` which blocks
the CPU until the GPU (or CPU tessellation engine) produces vertices.

Ellipses are particularly expensive: a manual CPU loop generates 96–4096
triangle segments based on radius, with no caching:

```cpp
// RenderTarget.cpp:929–953 — runs every frame for every ellipse
segmentCount = std::min(4096u, std::max(96u,
    std::ceil(std::max(rx, ry) * renderScale)));
for(unsigned i = 0; i < segmentCount; i++){
    // Build triangle fan vertex by vertex
}
```

**How production systems solve this:**

- **Chromium:** Avoids tessellation entirely for 2D compositing. Shapes
  are either pre-rasterized into tiles (cached as GPU textures) or
  drawn as axis-aligned quads with SDF evaluation in the fragment
  shader. A rounded rect is a quad + a shader, not a tessellated mesh.

- **Game engines:** Mesh geometry is uploaded once to device-local GPU
  memory and reused across frames by handle. Dynamic geometry uses ring
  buffers — a large pre-allocated vertex buffer divided into per-frame
  segments with no allocation, just pointer advancement.

### 3. Render target re-creation on every resize

During a live resize drag, `setRenderTargetSize()` is called for every
resize event (60–120/sec). Each call triggers `rebuildBackingTarget()`:

```cpp
void BackendRenderTargetContext::rebuildBackingTarget(){
    renderTarget->waitForGPU();         // BLOCK: wait for in-flight work
    texturePool->release(targetTexture);
    texturePool->release(effectTexture);
    targetTexture = texturePool->acquire(poolKey);   // may allocate
    effectTexture = texturePool->acquire(poolKey);   // may allocate
    preEffectTarget = gte.graphicsEngine->makeTextureRenderTarget({...});
    effectTarget = gte.graphicsEngine->makeTextureRenderTarget({...});
    tessellationEngineContext = gte.triangulationEngine->createTEContextFromTextureRenderTarget(preEffectTarget);
}
```

This destroys and recreates **six GPU objects** (2 textures, 2 render
targets, 1 tessellation context, plus the old objects released) on every
resize event, with a synchronous GPU wait.

**How production systems solve this:**

- **Chromium:** Render target textures are managed by
  `SharedImagePool`, keyed by (size, format). Textures that match are
  reused without allocation. The swap chain itself is a fixed 2–3
  buffer rotation — never re-created during resize. Content is
  rendered to the closest tile size and scaled if needed during a
  resize drag, then re-rendered at final resolution on drag end.

- **Game engines:** Transient resources are allocated from a pool with
  size tolerance. A render target request for 800×600 can reuse an
  existing 1024×768 texture (rendering into a sub-region). Textures are
  pooled by power-of-two size buckets. The existing `TexturePool`
  allows 1.5× oversize reuse, but during a resize storm the constant
  size changes cause constant pool misses because adjacent resize
  events differ by only a few pixels — always outside the 1.5×
  tolerance of the *previous* size.

### 4. No geometry caching

A `CanvasFrame` contains a list of `VisualCommand` objects, each
describing a shape with parameters (rect, brush, border, etc.). When
the same widget paints the same shapes at the same sizes across frames,
every shape is re-tessellated from scratch. There is no cache keyed by
shape parameters.

### 5. No dirty region tracking at the render level

When a `CanvasFrame` arrives, the entire layer texture is cleared and
all visual commands are re-rendered, even if only one command changed
(e.g. a text label updated while the surrounding shapes are identical).
There is no mechanism to diff the current frame against the previous
one and skip unchanged commands.

### 6. Console logging on the hot path

`std::cout` calls exist throughout `renderToTarget()` — on every visual
command entry, tessellation result, buffer bind, and render pass
start/end. Additionally, JSON diagnostic logging writes to
`../../../debug-85f774.log` on every command. On Windows, console output
is particularly expensive due to the console subsystem's synchronous
write model.

---

## Architectural root cause: the compositor is a command processor, not a frame compositor

The per-command inefficiencies above (root causes 1–6) are serious, but
they are multiplied by a deeper architectural problem: the compositor
has no concept of a **frame**. It processes individual widget packets
one at a time, serially, presenting after each one.

### The current scheduling loop

The `CompositorScheduler` runs a single background thread:

```
while(true):
    pop one command from priority queue         // Compositor.cpp:408
    processCommand(command)                     // one widget's render work
    renderTargetStore.presentAllPending()        // present ALL dirty targets
    update telemetry / decrement inFlight        // Compositor.cpp:424–450
    if queue empty: onQueueDrained()             // Compositor.cpp:453
```

Each iteration processes **one packet** (one widget's `CanvasFrame`),
fully renders it (all visual commands, including per-command GPU
submissions), and then presents. The next widget's packet is processed
in the next iteration.

### What happens during a resize with 10 widgets

1. Platform fires a resize event.
2. `WidgetTreeHost::notifyWindowResize()` calls
   `root->handleHostResize()`.
3. `handleHostResize` recursively resizes all widgets and calls
   `invalidate(PaintReason::Resize)` on each one.
4. Each widget's `invalidate()` calls `executePaint()`, which runs
   `startCompositionSession()` → `onPaint()` → `endCompositionSession()`.
5. `endCompositionSession()` calls `CompositorClientProxy::submit()`,
   which packages the widget's frame into a `CompositorPacketCommand`
   and calls `Compositor::scheduleCommand()`.

**Result:** 10 widgets produce 10 separate packets in the queue, all
from a single resize event.

6. The scheduler pops packet 1, renders widget 1's frame (39–95ms per
   the earlier analysis), calls `presentAllPending()`.
7. **The display now shows widget 1 at the new size but widgets 2–10
   at the old size.** This is the staggered rendering the user observes.
8. The scheduler pops packet 2, renders widget 2, presents again.
9. Repeat for packets 3–10.

**Total time to display the complete resized frame:** 10 × 50ms = 500ms
(at the fast end). During this 500ms, the display shows partial updates
as each widget catches up one by one.

### Queue flooding arithmetic

If resize events arrive at 60 Hz (once per vsync) and each event
produces 10 packets, that is 600 packets/second entering the queue.
The scheduler processes them at approximately 10–20 packets/second
(50–100ms each). The queue grows by ~580 packets/second.

After a 1-second resize drag, the queue contains approximately 580
unprocessed packets. At 50–100ms each, the queue takes **29–58 seconds**
to drain. This matches the user's observation of command buffer
completion logs appearing long after the resize finishes.

The "1–2 second wait gap" between log bursts corresponds to the queue
temporarily draining (the scheduler catches up during a pause in resize
events), sleeping on `queueCondition.wait()`, then waking when the next
resize burst queues more packets.

### Why this architecture cannot be fixed incrementally

Even if per-command rendering were instantaneous (0ms per command),
the scheduler's serial packet processing and per-packet presentation
would still produce staggered partial updates. Widget 1 would still be
presented before widget 2, creating a visible flash of inconsistent
content. The fundamental issue is that **there is no point in the
pipeline where all widgets' contributions are collected before any
of them are presented**.

### How production compositors solve this

**Chromium (`viz::Display`):** Uses a **surface mailbox** architecture.
Each producer (renderer process, browser UI) has a `Surface` — a
single-buffered slot that holds the latest `CompositorFrame`. Producers
submit to their surface independently. The `DisplayScheduler` runs on
vsync and, at frame time, calls `SurfaceAggregator::Aggregate()` which
reads the latest frame from *every* surface in one pass, merges them
into a single `AggregatedFrame`, and presents once. If a producer hasn't
submitted a new frame, the aggregator uses the previous one — stale
content is better than missing content. This means:

- **No serial processing.** All surfaces are read in parallel (logically).
- **One present per vsync.** Never per-widget.
- **Decoupled submission from presentation.** A producer submitting a
  frame does not trigger any rendering or presentation. That happens on
  the display compositor's schedule.

For resize specifically, Chromium uses `LocalSurfaceId` generation
tracking: when a resize occurs, a new `LocalSurfaceId` is allocated.
The compositor knows whether each surface has "caught up" to the current
size by comparing its `LocalSurfaceId` against the expected one. Until
all surfaces have caught up, the compositor can show the old content
(stretched or padded) rather than presenting a partial update.

**Game engines:** Use a **gather → render → present** loop. All
rendering sources (3D world, UI overlay, debug overlay) submit draw
commands to a shared command buffer or render queue during the "gather"
phase. No source has access to the present call. The frame loop renders
all gathered commands and presents once at the end. There is no path
from an individual source to presentation — only the frame loop can
present.

---

## Proposed architecture

The plan is structured in three tiers. **Tier 0** consolidates the
native view tree to one native view per window with a virtual widget
tree — the foundational change that makes the compositor redesign
possible. NV-1 (root-only native view) and NV-2 (virtual hit testing)
are complete; NV-3 (single-surface rendering) is the next milestone.
**Tier 1** (Phases A–C) addresses the compositor scheduling
architecture — the frame model, collection strategy, and present
discipline, now operating on a single surface per window. **Tier 2**
(Phases 0–5) addresses per-command rendering efficiency within the new
architecture. The tiers are ordered by dependency, but can be
implemented incrementally within each tier.

### Tier 0: Native view consolidation

The full rationale and migration plan are in
`Native-View-Architecture-Plan.md`. This section covers the aspects
that directly affect the compositor and rendering architecture.

#### The previous model and why it failed

Prior to NV-1/NV-2, each Widget owned a View, and each View created a
native platform view. This meant N widgets = N native views = N compositor
surfaces = N render targets = N GPU textures = N present operations.

**NV-1 (COMPLETED)** eliminated the native view tree: `addNativeItem()`
was removed from `NativeWindow`, `WindowLayer` was deleted, and child
Views became virtual. Views still create NativeItem pointers for render
target access — those are removed in NV-3.

**NV-2 (COMPLETED)** restored event dispatch: `WidgetTreeHost::hitTest()`
walks the virtual widget tree in reverse z-order to route input events
from the single root native view to the correct virtual View.

**Remaining problem:** each View still has its own render target and
compositor surface. N widgets still = N compositor surfaces = N render
targets = N GPU textures = N present operations. NV-3 (single-surface
rendering) eliminates this.

#### The target model

One native view per window. All child widgets are virtual — they have
bounds, can paint, and can receive events, but they have no native
platform view. This is what every production toolkit has converged on:

| Toolkit | Year | Approach |
|---------|------|----------|
| WPF | 2006 | Single HWND, DirectX rendering |
| Qt 4.4 | 2008 | "Alien widgets" — one native window per toplevel |
| Chromium | 2009 | One NSView/HWND, Skia painting, virtual View tree |
| Flutter | 2017 | Single surface, Skia rendering |
| GTK4 | 2020 | Single GdkSurface, scene graph |
| WinUI 3 | 2021 | Single HWND, windowless controls |

Chromium creates one `BridgedContentView` (NSView) per window on macOS,
one HWND per window on Windows, and one X11/Wayland surface on Linux.
All browser chrome UI is a virtual `views::View` tree painted onto
that single surface. `views::View` objects have no native backing.
Hit testing, event routing, and painting are all custom.

#### Impact on the compositor

With one native view per window, the rendering architecture simplifies
fundamentally:

| Aspect | Current (N native views) | Proposed (1 native view) |
|--------|--------------------------|--------------------------|
| Compositor surfaces | N (one per widget) | 1 (one per window) |
| Render targets | N | 1 |
| GPU textures | N | 1 |
| Present operations per frame | N (staggered) | 1 |
| Render passes per frame | N × M (M commands per widget) | 1 (all commands, all widgets) |
| Resize render target rebuilds | N | 1 |
| Compositor surface mailboxes | N deposits + N consumes | 1 deposit + 1 consume |

This transforms Tier 1's surface mailbox from a per-widget architecture
to a per-window architecture. Instead of N `CompositorSurface` objects
that the compositor must aggregate, there is one surface per window.
The frame loop reads one surface, renders all widgets' commands in one
render pass, and presents once.

#### Phases (from Native View Architecture Plan)

1. **Root-only native view [COMPLETED]** — only the window-level View
   creates a native item. Child Views are virtual. `addSubView()` adds
   to a virtual child list instead of calling `addChildNativeItem()`.
   `WindowLayer` deleted. `addNativeItem()` removed from `NativeWindow`.
   NativeItem pointers remain on Views for render target access —
   full removal deferred to NV-3.

2. **Virtual hit testing [COMPLETED]** — the root native view receives
   all events. `WidgetTreeHost::hitTest()` walks the virtual widget
   tree (reverse z-order) to find the deepest `View` under the cursor.
   `AppWindowDelegate` routes input events (mouse, keyboard) through
   `WidgetTreeHost::dispatchInputEvent()`. Hover tracking synthesizes
   `CursorEnter`/`CursorExit` when the hovered view changes. Root
   NativeItem's `event_emitter` wired to `AppWindow` in `Impl`
   constructor.

3. **Single-surface rendering [COMPLETED]** — all widgets render into
   the root view's single backing surface via `setViewportOverride()`.
   Child Views share the root visual's `BackendRenderTargetContext`;
   `ensureLayerSurfaceTarget()` resolves child layers to the root
   visual's surface. This is where Tier 0 connects to Tier 1: the
   single surface per window is the surface mailbox for the frame loop.

4. **Coordinate system unification [COMPLETED]** — top-left origin is
   now standard across all platforms. macOS: `isFlipped` returns YES.
   Windows: no Y-inversion. Widget.h documents the convention. Hit
   testing uses top-left coordinates throughout.

5. **Native view embedding** — `NativeViewHost` escape hatch for
   widgets that genuinely need a native view (IME text input, video
   players).

---

### Tier 1: Compositor scheduling architecture

#### Phase A — Surface mailbox abstraction [COMPLETED]

**Goal:** Decouple widget submission from compositor rendering. Widgets
deposit frames into a mailbox; the compositor reads the mailbox on its
own schedule.

With Tier 0's single-native-view model, the surface mailbox simplifies
from one-per-widget to one-per-window. The `CompositorSurface` is a
per-window slot that accumulates all widgets' draw commands into a
single frame:

```cpp
class CompositorSurface {
    /// The latest composite frame — contains draw commands from ALL
    /// widgets in this window's virtual tree. Built by the main thread
    /// during the widget paint pass. Read by the compositor during
    /// the render phase.
    std::mutex mutex;
    SharedHandle<CompositeFrame> latestFrame;
    uint64_t generation = 0;     // Incremented on each deposit
    uint64_t sizeGeneration = 0; // Incremented on resize

    /// The previously rendered frame. Used for dirty comparison and
    /// stale-content fallback.
    SharedHandle<CompositeFrame> renderedFrame;
    uint64_t renderedGeneration = 0;

    /// The committed GPU texture from the last render. Reused when
    /// no new frame is available (stale content fallback).
    SharedHandle<OmegaGTE::GETexture> committedTexture;

public:
    /// Called after the widget tree paint pass completes. Deposits the
    /// composite frame (all widgets' commands) into the mailbox.
    /// Does NOT trigger rendering or presentation.
    void deposit(SharedHandle<CompositeFrame> frame);

    /// Called by the compositor's render phase. Returns the latest
    /// frame if it differs from the previously rendered one, or nullptr
    /// if no update is needed.
    SharedHandle<CompositeFrame> consume();

    /// Returns true if a new frame has been deposited since the last
    /// consume().
    bool hasPendingUpdate() const;
};
```

The `CompositeFrame` replaces the per-widget `CanvasFrame` model. When
`WidgetTreeHost` completes a paint pass (all widgets have painted in
response to an invalidation or resize), it collects all widgets' draw
commands into a single `CompositeFrame` and deposits it into the
window's `CompositorSurface`.

```cpp
struct CompositeFrame {
    /// Draw commands from all widgets, ordered by z-order (back to front).
    /// Each entry identifies the widget, its bounds in window coordinates,
    /// and its list of visual commands.
    struct WidgetSlice {
        Composition::Rect bounds;          // Widget bounds in window coordinates
        OmegaCommon::Vector<VisualCommand> commands;
        uint64_t contentHash = 0;   // For per-widget dirty detection
    };
    OmegaCommon::Vector<WidgetSlice> slices;
    uint64_t sizeGeneration = 0;    // Resize generation
};
```

**This is the key architectural change:** the main thread's
`invalidate()` → `executePaint()` → `submit()` path no longer produces
compositor commands or per-widget surfaces. It builds a composite frame
containing all widgets' commands, then deposits it into the single
window-level mailbox. The compositor reads the mailbox on its own
schedule.

**Prior art:** Chromium `Surface` / `SurfaceManager`. Each renderer
process deposits `CompositorFrame`s into its `Surface`. The display
compositor reads all surfaces during `SurfaceAggregator::Aggregate()`.
With Tier 0's single-window model, this simplifies to one surface per
window — analogous to a single-process Chromium renderer.

#### Phase A-1 — Direct-to-drawable rendering [COMPLETED]

**Goal:** Eliminate the intermediate offscreen texture and fullscreen-quad
blit. Render visual commands directly to the native drawable
(`CAMetalLayer` / DXGI swap chain). If a frame fails to render, the
previously presented content remains visible — the native presentation
layer handles this automatically.

**The problem:** The current pipeline renders to an RGBA8Unorm offscreen
texture via `BackendRenderTargetContext::beginFrame()`/`endFrame()` →
`commit()`. Then `compositeAndPresentTarget()` runs a second render pass
using the `finalCopyRenderPipelineState` shader to blit the texture onto
the BGRA8Unorm native drawable. This is:

1. An extra GPU render pass (the fullscreen-quad blit)
2. An extra texture allocation per visual (the offscreen `targetTexture`)
3. A pixel format mismatch requiring a separate copy shader pipeline
4. Dead complexity — `blitAndPresent()`, `compositeAndPresentTarget()`,
   `getFinalCopyPipelineForFormat()`, `finalCopyRenderPipelineState`,
   `finalTextureDrawBuffer`, copy vertex/fragment shaders

**The fix:** Render directly to the native drawable. The
`BackendRenderTargetContext` already holds a `GENativeRenderTarget`
reference (`renderTarget` member) — it is just never used for rendering
(always null for visual contexts). Wire it up.

##### A-1.1 Unify pixel format to BGRA8Unorm

The render pipeline states (`renderPipelineState`,
`textureRenderPipelineState`) are compiled with the default
`colorPixelFormat = RGBA8Unorm`. The native drawable uses BGRA8Unorm.
These are incompatible — Metal/Vulkan require the pipeline's color
attachment format to match the render pass attachment.

Change: compile all render pipeline states with BGRA8Unorm. This matches
the native drawable format. Effect textures that still need intermediate
allocation also switch to BGRA8Unorm for consistency.

**Files:**
- `RenderTarget.cpp` — `initGlobalRenderAssets()`: set
  `renderPipelineDescriptor.colorPixelFormat = BGRA8Unorm` before
  creating `renderPipelineState` and `textureRenderPipelineState`
- `RenderTarget.cpp` — `rebuildBackingTarget()`: change texture pool key
  and `TextureDescriptor.pixelFormat` from `RGBA8Unorm` to `BGRA8Unorm`
  (for effect-path textures that still need allocation)

##### A-1.2 Render to native drawable in `beginFrame()`/`endFrame()`

When a `BackendRenderTargetContext` has a non-null native render target
and no effects are queued, `beginFrame()` gets the command buffer from
the native target instead of the texture target. Drawing commands go
directly to the drawable.

```
beginFrame():
    if renderTarget != nullptr && effectQueue.empty():
        frameCB_ = renderTarget->commandBuffer()
        // render pass targets the native drawable directly
    else:
        frameCB_ = preEffectTarget->commandBuffer()
        // render pass targets the offscreen texture (effect path)

endFrame():
    // same as before — end pass, submit CB

commit():
    if rendered to native:
        renderTarget->commitAndPresent()    // present directly
        // no committedTexture, no hasPendingContent
    else:
        // existing effect path unchanged
```

##### A-1.3 Wire native target into visual contexts

Currently, `CALayerTree.mm` (and `DCVisualTree.cpp`) create
`BackendRenderTargetContext` with `nullptr` for the native target:

```cpp
// Current (CALayerTree.mm line ~137):
BackendRenderTargetContext compTarget(r, nullNative, scale);
```

Change: pass the actual `GENativeRenderTarget` from the
`ViewPresentTarget` to the root visual's `BackendRenderTargetContext`:

```cpp
// New:
BackendRenderTargetContext compTarget(r, nativeTarget, scale);
```

For body/child visuals: also pass the native target. All visuals in a
single-surface window share the same native drawable (Tier 0
established this — one surface per window).

**Files:**
- `wtk/src/Composition/backend/mtl/CALayerTree.mm` — pass native target
  to `BackendRenderTargetContext` constructor
- `wtk/src/Composition/backend/dx/DCVisualTree.cpp` — same change for
  D3D12

##### A-1.4 Remove the blit infrastructure

Once direct rendering is in place, the following become dead code:

- `blitAndPresent()` function
- `compositeAndPresentTarget()` — replaced by direct present in
  `commit()`
- `getFinalCopyPipelineForFormat()` and `finalCopyPipelinesByFormat` map
- `finalCopyRenderPipelineState` and `finalTextureDrawBuffer`
- `copyVertex` / `copyFragment` shader lookups in
  `initGlobalRenderAssets()`
- `committedTexture` field on `BackendRenderTargetContext` (unless
  retained for effect path)
- `hasPendingContent` flag (present happens immediately in `commit()`)
- `ViewPresentTarget` struct (native target now lives on the context)
- `RenderTargetStore::presentAllPending()` (present happens in
  `commit()`, not deferred)

**What stays for the effect path:** When effects are queued, the context
still renders to the offscreen texture, applies effects, then does a
single blit to native. This is the only case where the blit is
justified — effects need intermediate texture ping-ponging. A dedicated
`presentEffectResult()` method handles this narrow case.

##### Effect path detail

Effects (blur, shadow) require rendering to a texture first, applying
the effect as a compute/render pass, and then presenting the result.
For this case:

1. `beginFrame()` detects `!effectQueue.empty()` → renders to texture
2. `commit()` applies effects (existing ping-pong logic)
3. After effects: single blit from result texture to native drawable
4. `renderTarget->commitAndPresent()`

This keeps the effect path correct while eliminating the blit for the
common no-effect case.

##### Files touched

| File | Changes |
|------|---------|
| `RenderTarget.h` | Remove `committedTexture`, `hasPendingContent`. Remove `ViewPresentTarget`. Simplify `BackendCompRenderTarget` |
| `RenderTarget.cpp` | Change pixel format to BGRA8Unorm. Modify `beginFrame()`/`commit()` for direct native rendering. Remove `blitAndPresent()`, `compositeAndPresentTarget()`, blit pipeline setup. Keep effect-path blit |
| `Execution.cpp` | Remove `presentAllPending()` calls (present happens in `commit()`). Simplify `onQueueDrained()` |
| `mtl/CALayerTree.mm` | Pass native target to visual contexts |
| `dx/DCVisualTree.cpp` | Pass native target to visual contexts |

#### Phase B — Frame-oriented compositor loop

**Goal:** Replace the serial command-processing loop with a
frame-oriented loop that renders the window's composite frame and
presents once.

Replace the `CompositorScheduler`'s current loop:

```
Current:
    while(true):
        pop one packet                              // one widget's work
        processCommand(command)                     // render that widget
        renderTargetStore.presentAllPending()        // present
        telemetry
```

With a frame-oriented loop:

```
Proposed:
    while(true):
        wait for next frame trigger (vsync, surface damage, or timeout)

        // CONSUME: read the window's single surface
        compositeFrame = surface.consume()
        if compositeFrame == nullptr:
            continue    // no new content — previous frame stays visible

        // RENDER directly to the native drawable (Phase A-1)
        cb = nativeRenderTarget->commandBuffer()
        cb->startRenderPass(renderPassDesc)

        for each widgetSlice in compositeFrame.slices:
            cb->setScissorRect(widgetSlice.bounds)
            for each command in widgetSlice.commands:
                renderToTarget(cb, command)   // Tier 2 optimisations apply

        cb->endRenderPass()
        nativeRenderTarget->submitCommandBuffer(cb)   // ONE GPU submission

        // PRESENT: one atomic present to the drawable
        nativeRenderTarget->commitAndPresent()

        // TELEMETRY
        updateTelemetry()
```

The critical properties of this loop:

1. **One surface, one consume.** With Tier 0's single native view per
   window, there is exactly one `CompositorSurface` to read. No
   aggregation of multiple surfaces is needed. The composite frame
   already contains all widgets' commands in z-order.

2. **One render pass, one GPU submission.** All widgets' visual commands
   from all `WidgetSlice`s are recorded into a single open render pass
   targeting the native drawable directly (Phase A-1). This is the
   maximum possible batching — the entire window's content is one GPU
   submission. No intermediate textures, no blit pass.

3. **One present per iteration.** Never per-widget, never per-command.
   The swapchain present happens once, after all widgets' commands have
   been rendered into the single surface.

4. **Stale content fallback.** If no new composite frame is available
   (no widget invalidated since last present), the compositor simply
   does not present — the native drawable retains the previously
   presented content automatically (Metal/D3D12 behavior). With Phase
   A-1's direct-to-drawable rendering, there is no intermediate
   `committedTexture` to manage. The display always shows complete
   content.

5. **Frame trigger.** The loop runs on a trigger rather than draining a
   queue. Trigger sources:
   - **Surface damage:** The window's surface receives a new deposit →
     schedule a frame. Debounce to avoid running faster than vsync.
   - **Vsync:** On platforms with display link access (`CVDisplayLink`
     on macOS, `IDXGIOutput::WaitForVBlank` on Windows), align frame
     production to vsync for tear-free presentation.
   - **Timeout:** Fallback for platforms without vsync — run at a target
     interval (e.g. 16.67ms).

**Prior art:**

- Chromium `DisplayScheduler`: runs on vsync, sends `BeginFrame` to all
  clients, waits for responses (or a deadline), then calls
  `Display::DrawAndSwap()` which aggregates all surfaces and presents.
- Game engine frame loop: sequential phases (update → gather → render →
  present), one iteration per frame, single present. All rendering
  sources submit to a shared command buffer — no source can trigger
  presentation independently.

#### Phase C — Resize coordination

**Goal:** During a resize, produce a single coherent frame at the new
size without staggered partial updates.

With Tier 0's single-surface model, resize coordination simplifies
dramatically. There is no multi-surface synchronization problem — all
widgets render into one composite frame. The resize path becomes:

1. Platform fires a resize event.
2. `WidgetTreeHost::notifyWindowResize()` relayouts the virtual widget
   tree and triggers a full paint pass (all widgets repaint at the new
   size). This happens synchronously on the main thread.
3. The paint pass produces a single `CompositeFrame` with all widgets'
   commands at the new dimensions.
4. The `CompositeFrame` is deposited into the window's single
   `CompositorSurface` with an incremented `sizeGeneration`.
5. The compositor's frame loop consumes the frame, renders all commands
   directly to the native drawable in one pass, and presents. With
   Phase A-1's direct-to-drawable rendering, there is no offscreen
   texture to resize — the native drawable is managed by the platform
   (`CAMetalLayer` auto-sizes on macOS, swap chain resize on Windows).
   Effect-path textures still use bucketed dimensions (Tier 2 Phase 3)
   when effects are active.

```cpp
struct ResizeState {
    uint64_t generation = 0;         // Incremented on each resize
    Composition::Rect pendingSize {};       // The target size
    bool renderTargetStale = false;  // True when generation > rendered generation
};
```

The key simplification: because the paint pass is synchronous and
produces a complete composite frame, there is no waiting for individual
surfaces to "catch up." Either the composite frame is at the current
generation (all widgets painted at the new size) or it isn't (stale
content fallback). There is no partial state.

**Render target management during resize:**

With Phase A-1's direct-to-drawable rendering, the native drawable is
resized by the platform automatically — `CAMetalLayer` tracks the view
bounds on macOS, and the swap chain is resized on Windows. There is no
`rebuildBackingTarget()` call for the common no-effect path. For widgets
using effects, the offscreen effect textures still use bucketed
dimensions (Tier 2 Phase 3), but this is the uncommon case.

**Prior art:** Chromium `LocalSurfaceId` uses generation tracking for
multi-surface resize coordination. With OmegaWTK's single-surface model,
the equivalent is simpler — just a generation counter on the one
surface. Game engines resize their single render target (or swap chain)
once per resize event, with no per-widget coordination needed.

#### What happens to the existing command queue

The priority queue (`commandQueue`), `CompositorCommand` hierarchy, and
`CompositorScheduler` are replaced by the surface mailbox and
frame-oriented loop. The command types map to the new architecture:

| Current command type | New equivalent |
|---------------------|----------------|
| `CompositionRenderCommand` | `WidgetSlice` in `CompositeFrame` |
| `CompositorPacketCommand` | Eliminated — the composite frame is the grouping unit |
| `CompositorLayerCommand` (resize) | Eliminated — single surface, no per-widget layers |
| `CompositorViewCommand` (resize) | Eliminated — single native view per window |
| `CompositorCancelCommand` | Eliminated — surface automatically shows latest |
| Priority ordering | Eliminated — z-ordered slices in one composite frame |
| Lane admission | Replaced by frame trigger debouncing |
| Sync lanes | Eliminated — one surface per window, one generation counter |
| Per-widget render targets | Eliminated — one render target per window |
| `CompositorClientProxy` | Simplified — widget deposits commands into `CompositeFrame` builder |

The `CompositorClient` / `CompositorClientProxy` classes are simplified:
instead of managing a command queue, command IDs, sync lane IDs, and
recording depth, the proxy holds a reference to a `CompositeFrame`
builder. When `endCompositionSession()` is called, the widget's commands
are appended as a `WidgetSlice`. When the paint pass completes, the
composite frame is deposited into the window's single surface.

---

### Tier 2: Per-command rendering efficiency

These phases apply within the "RENDER" step of the new frame loop —
where the composite frame's widget slices are rendered into the window's
single render target. With Tier 0's single-surface model, all widgets'
commands are in one render pass, so these optimizations have maximum
impact: batched submission covers ALL widgets' commands (not just one
widget's), geometry caching covers the entire window, and frame diffing
can skip unchanged widget slices.

### Phase 0 — Remove hot-path logging [COMPLETED]

**Goal:** Establish a clean performance baseline before optimising.

**Files:** `RenderTarget.cpp`, `Execution.cpp`

All `std::cout` calls in `renderToTarget()`, `commit()`,
`rebuildBackingTarget()`, `getFinalCopyPipelineForFormat()`,
`ensureLayerSurfaceTarget()`, and `executeCurrentCommand()` are now
gated behind `#ifdef OMEGAWTK_TRACE_RENDER`. All `debug-85f774.log`
JSON file writes (`#region agent log` blocks) have been removed.
The `#include <fstream>` was removed from both files.

---

### Phase 1 — Batched GPU submission [COMPLETED]

**Goal:** Reduce GPU submissions per frame from N (one per visual
command) to 1.

**The core change:** Instead of starting and ending a render pass for
each visual command, a single render pass is opened at the beginning of
the frame, all draw calls are recorded into it, and it is ended/submitted
once at the end.

#### Implemented API

Two new methods on `BackendRenderTargetContext`:

```cpp
/// Open a frame-level render pass that clears to the given color.
/// All subsequent renderToTarget() calls record into this pass.
void beginFrame(float clearR, float clearG, float clearB, float clearA);

/// Close the frame-level render pass and submit the command buffer.
void endFrame();
```

The call site in `executeCurrentCommand()` changed from:

```
Before:
    targetContext->clear(bkgrd.r,bkgrd.g,bkgrd.b,bkgrd.a);
    for each visual command:
        targetContext->renderToTarget(type, params)
            ← each one: create CB, start pass, draw, end pass, submit

After:
    targetContext->beginFrame(bkgrd.r, bkgrd.g, bkgrd.b, bkgrd.a);
    for each visual command:
        targetContext->renderToTarget(type, params)
            ← records draws into open pass using frameCB_
    targetContext->endFrame();
        ← ONE endRenderPass + ONE submitCommandBuffer
```

Private members added to `BackendRenderTargetContext`:

```cpp
SharedHandle<OmegaGTE::GERenderTarget::CommandBuffer> frameCB_;
bool frameActive_ = false;
bool lastPipelineWasTexture_ = false;
```

#### 1.1 Single render pass per frame

`beginFrame()` creates one command buffer, starts a render pass with
`Clear` load action (merging the former separate `clear()` call), and
sets viewport/scissor for the frame. `endFrame()` ends the render pass
and submits.

`renderToTarget()` uses the open `frameCB_` instead of creating its own
command buffer. It no longer starts/ends render passes or submits.
A standalone fallback path is preserved for any call outside a
`beginFrame()`/`endFrame()` pair.

The `renderToTarget()` signature is unchanged — it accesses the open
command buffer via the `frameCB_` member rather than accepting a
parameter.

#### 1.2 Pipeline state batching

A `lastPipelineWasTexture_` flag tracks the currently bound pipeline.
`setRenderPipelineState()` is only called when the pipeline type changes
between consecutive commands (color vs. texture). Reset on each
`beginFrame()` and after any render pass restart (texture fence handling).

#### 1.3 Vertex buffer batching (deferred)

Per-command vertex buffer acquisition from `BufferPool` is retained.
Each command acquires its own buffer, writes vertices, flushes, binds,
and draws. A single shared vertex buffer per frame (ring buffer pattern)
is deferred to a future phase.

#### Texture fence handling

When a command has a texture fence (rare — only pre-created bitmap
textures), the open render pass is temporarily ended, the fence is
registered via `notifyCommandBuffer()`, and the render pass is restarted
with `LoadPreserve`. Viewport/scissor and pipeline state are re-set
after restart.

#### Other changes

- The per-packet clear deduplication (`s_lastClearedContext`,
  `s_lastClearedRenderTarget`, `resetLastClearedForNextBatch()`) was
  removed — `beginFrame()` always clears.
- `bufferWriter->flush()` moved before draw calls (was after in old code)
  for correct ordering within the batched pass.

#### Files touched

| File | Changes |
|------|---------|
| `RenderTarget.h` | Added `beginFrame()`, `endFrame()`, `frameCB_`, `frameActive_`, `lastPipelineWasTexture_` |
| `RenderTarget.cpp` | Implemented `beginFrame()`/`endFrame()`. Restructured `renderToTarget()` to record into open pass. Pipeline state tracking. Standalone fallback. Texture fence end/restart |
| `Execution.cpp` | Replaced clear + command loop with `beginFrame()`/loop/`endFrame()`. Removed clear dedup statics and `resetLastClearedForNextBatch()` |

---

### Phase 2 — Geometry caching

**Goal:** Eliminate redundant tessellation. Only tessellate a shape when
its parameters change.

#### 2.1 Tessellation cache

Introduce a `TessellationCache` that maps shape parameters to cached
tessellation results:

```cpp
struct ShapeKey {
    VisualCommand::Type type;
    // Shape-specific parameters:
    float x, y, w, h;          // Bounds
    float radiusX, radiusY;     // Corner radii (rounded rect)
    float strokeWidth;          // Path stroke
    // ... etc
    bool operator==(const ShapeKey &) const;
};

struct CachedTessellation {
    OmegaGTE::TETessellationResult result;
    uint64_t frameLastUsed = 0;  // For LRU eviction
};

class TessellationCache {
    std::unordered_map<ShapeKey, CachedTessellation, ShapeKeyHash> cache;
    static constexpr size_t kMaxEntries = 1024;
public:
    /// Returns cached result or nullptr.
    const CachedTessellation * find(const ShapeKey & key) const;
    /// Insert a new result. Evicts LRU if at capacity.
    void insert(const ShapeKey & key, OmegaGTE::TETessellationResult && result);
    /// Mark a key as used this frame (update LRU).
    void touch(const ShapeKey & key, uint64_t frameId);
    /// Evict entries not used in the last N frames.
    void evict(uint64_t currentFrameId, uint64_t maxAge = 60);
};
```

The cache lives on `BackendRenderTargetContext` (per-layer). Before
calling `triangulateSync()`, build the `ShapeKey` from the command
parameters and check the cache. On hit, skip tessellation entirely and
use the cached vertex data.

**Prior art:** Game engines upload mesh geometry once and reference it by
handle forever. Chromium caches rasterized tiles and only re-rasterizes
tiles whose display items changed.

#### 2.2 Ellipse and shadow vertex caching

The manual CPU triangle-fan generation for ellipses (lines 929–953) and
shadows is a special case — it doesn't go through `triangulateSync()`.
Apply the same cache to these: hash (center, radiusX, radiusY,
segmentCount) → cached vertex array.

#### 2.3 Cached vertex buffer reuse

When a tessellation cache hit occurs, the previously written vertex data
may still exist in a GPU buffer from a prior frame. If the buffer hasn't
been recycled by the pool, the draw call can reference it directly
without any CPU vertex writing. This requires tagging cached entries with
their buffer and offset, and extending the buffer pool to support
"pinned" buffers that are not recycled while cache entries reference them.

#### Files touched

| File | Changes |
|------|---------|
| `RenderTarget.h` | Add `TessellationCache` to `BackendRenderTargetContext` |
| `RenderTarget.cpp` | Cache lookup before `triangulateSync` in all shape branches. Cache insert after tessellation. Skip vertex write on buffer reuse |
| New file: `TessellationCache.h` | `ShapeKey`, `CachedTessellation`, `TessellationCache` |

---

### Phase 3 — Effect-path texture resize tolerance

**Goal:** Eliminate GPU allocation on every resize event for the effect
path. (The common no-effect path renders directly to the native drawable
after Phase A-1 and does not allocate offscreen textures at all.)

**Scope change from Phase A-1:** Before Phase A-1, every visual rendered
to an offscreen texture and `rebuildBackingTarget()` ran on every resize,
making this phase critical. After Phase A-1, the offscreen texture path
only applies when effects (blur, shadow) are active. The optimization
still matters for effect-heavy UIs but has reduced scope.

#### 3.1 Bucketed effect texture dimensions

When effects are queued and the context falls back to texture rendering,
round the texture dimensions up to the nearest 64-pixel bucket instead
of allocating at exact pixel dimensions. The effect texture is reused
across resize events that stay within the same bucket.

```cpp
static unsigned roundToBucket(unsigned dim) {
    // Round up to next multiple of 64, minimum 64
    return std::max(64u, (dim + 63u) & ~63u);
}
```

With 64-pixel buckets, a resize from 800×600 to 805×603 reuses the
same 832×640 effect texture — no reallocation.

**Prior art:** Chromium's `TileManager` uses fixed tile sizes (256×256
or 512×512). Game engines allocate render targets at power-of-two or
next-multiple sizes and render into sub-regions.

#### 3.2 Deferred tessellation context recreation

The tessellation context is derived from the render target dimensions.
If the backing texture didn't change (same bucket), the tessellation
context can be reused. Only recreate it when the actual backing texture
changes.

#### 3.3 Deferred texture release

When effect textures do get reallocated (bucket boundary crossing),
move old textures to a "pending release" list instead of calling
`waitForGPU()` synchronously. The GPU signals completion via fences and
the textures return to the pool asynchronously.

```cpp
void BackendRenderTargetContext::rebuildBackingTarget(){
    if(targetTexture){
        deferredReleases.push_back({std::move(targetTexture), poolKey});
    }
    if(effectTexture){
        deferredReleases.push_back({std::move(effectTexture), poolKey});
    }
    targetTexture = texturePool->acquire(bucketedPoolKey);
    effectTexture = texturePool->acquire(bucketedPoolKey);
}
```

#### Files touched

| File | Changes |
|------|---------|
| `RenderTarget.cpp` | Bucketed dimensions for effect textures. Deferred release instead of `waitForGPU()`. Tessellation context reuse when backing unchanged |
| `RenderTarget.h` | Add deferred release queue to `BackendRenderTargetContext` |
| `TexturePool.h` | Add bucket-aware acquire that accepts a size range |

---

### Phase 4 — Frame diffing and partial render

**Goal:** Only re-render visual commands that changed since the previous
frame.

#### 4.1 Command-level diffing

When a new `CanvasFrame` arrives, compare it against the previously
rendered frame for the same layer:

- If the command count is the same and each command has identical type
  and parameters (position, size, brush, border), the frame is
  identical — skip rendering entirely and reuse the previous texture.
- If only some commands changed, identify the bounding rect of the
  changed region and only clear + re-render within that region (using
  scissor rect).

The diff needs a fast equality check per `VisualCommand`. Add an
`operator==` or a hash to `VisualCommand::Data` that compares the
shape-specific parameters. The hash can be computed at record time
(in `Canvas::draw*` methods) to avoid per-frame work.

#### 4.2 Per-frame command hash

Add a rolling hash to `CanvasFrame` that is updated as each visual
command is recorded:

```cpp
struct CanvasFrame {
    // ... existing fields ...
    uint64_t contentHash = 0;  // Rolling hash of all visual commands
};
```

When a frame arrives at the compositor, compare its `contentHash`
against the previous frame's hash for the same layer. If equal, the
frame is a no-op — skip rendering entirely.

**Prior art:** Chromium's `cc::DisplayItemList` records display items
and diffs them against the previous frame. Only tiles whose display
items changed are re-rasterized. The `cc::TileManager` tracks per-tile
invalidation rects.

#### Files touched

| File | Changes |
|------|---------|
| `Canvas.h` | Add `contentHash` to `CanvasFrame` |
| `Canvas.cpp` | Update `contentHash` in each `draw*` method |
| `RenderTarget.h` | Add `lastRenderedHash` to `BackendRenderTargetContext` |
| `RenderTarget.cpp` | Compare hash before rendering. Skip identical frames |
| `Execution.cpp` | Pass previous hash through to render target context |

---

### Phase 5 — SDF-based primitive rendering (future)

**Goal:** Eliminate tessellation entirely for standard shapes.

Instead of tessellating rounded rects, ellipses, and rects into triangle
meshes, draw a single quad (2 triangles, 6 vertices) and evaluate the
shape analytically in the fragment shader using signed distance functions
(SDFs).

A rounded rect SDF fragment shader:

```glsl
float roundedRectSDF(vec2 p, vec2 halfSize, float radius) {
    vec2 d = abs(p) - halfSize + vec2(radius);
    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - radius;
}

// In fragment main:
float dist = roundedRectSDF(fragCoord - center, halfSize, cornerRadius);
float alpha = 1.0 - smoothstep(-0.5, 0.5, dist);  // Anti-aliased edge
fragColor = shapeColor * alpha;
```

Benefits:
- **Zero tessellation.** Every shape is 6 vertices regardless of
  complexity.
- **Resolution-independent anti-aliasing.** The smoothstep produces
  perfect edges at any zoom level without increasing segment counts.
- **Trivially instancable.** All shapes of the same type can be drawn
  in one instanced draw call with per-instance data (bounds, color,
  corner radius).

With SDF rendering, the entire rendering path for a frame of 30 shapes
becomes: write 30 instance records (bounds + color + radius) into a
buffer, bind the SDF shader, issue one instanced draw call for 30
instances × 6 vertices = 180 vertices. One draw call, one submission,
zero tessellation.

**Prior art:** Chromium uses SDF-based clipping for rounded rects in
the compositor (shapes are quads, rounding is done in the shader).
Game engine UI systems (Unreal Slate, Unity UGUI) increasingly use
SDF rendering for resolution-independent UI elements. Mapbox GL uses
SDF rendering for all text and icon rendering.

This phase is deferred because it requires new shaders and changes the
rendering model significantly. Phases 1–4 provide large gains within the
existing tessellation-based model.

#### Files touched

| File | Changes |
|------|---------|
| `RenderTarget.cpp` | New SDF shader source. New render pipeline states. SDF branch in `renderToTarget` for Rect, RoundedRect, Ellipse |
| `RenderTarget.h` | SDF pipeline state handles. Instance buffer management |
| OmegaGTE shader source | SDF vertex + fragment shaders |

---

## Phase dependency graph

```
Tier 0 (Native View Consolidation):

NV-1: Root-only native view [COMPLETED]
    └─→ NV-2: Virtual hit testing [COMPLETED]
    └─→ NV-3: Single-surface rendering [COMPLETED] ──→ Tier 1
    └─→ NV-5: Native view embedding (independent, as needed)
NV-3 ──→ NV-4: Coordinate system unification [COMPLETED]

Tier 1 (Scheduling Architecture) — requires Tier 0 NV-3:

Phase A: Surface mailbox (single per-window surface) [COMPLETED]
    └─→ Phase A-1: Direct-to-drawable rendering (remove offscreen blit) [COMPLETED]
            └─→ Phase B: Frame-oriented compositor loop
                    └─→ Phase C: Resize coordination

Tier 2 (Per-Command Rendering) — operates within Phase B's render step:

Phase 0: Remove hot-path logging [COMPLETED]
    └─→ Phase 1: Batched GPU submission [COMPLETED]
            └─→ Phase 2: Geometry caching (second highest impact)
                    └─→ Phase 4: Frame diffing (builds on cache infrastructure)
                            └─→ Phase 5: SDF rendering (replaces tessellation entirely)
    └─→ Phase 3: Effect-path texture resize tolerance (independent of Phase 1)

Cross-tier:

Tier 0 NV-3 ──→ Tier 1 Phase A (single surface enables per-window mailbox)
Phase A ──→ Phase A-1 (direct rendering removes blit overhead)
Phase A-1 ──→ Phase B (frame loop renders directly to drawable)
Phase B ──→ Phase C (resize coordination requires frame loop)
Phase A-1 reduces Phase 3's scope (no offscreen texture for common path)
Phase 0 can begin in parallel with Tier 0/1 (logging removal is local to render path)
```

**Tier 0** is the foundational change. NV-1 (root-only native view),
NV-2 (virtual hit testing), NV-3 (single-surface rendering), and NV-4
(coordinate system unification) are complete. All child Views render
into the root visual's shared surface via viewport override. NV-5
(native view embedding) is an independent escape hatch.

**Tier 1** depends on Tier 0 NV-3. Phase A → A-1 → B are sequential.
Phase C depends on Phase B. Phase A-1 eliminates the offscreen texture
blit, which simplifies Phase B's rendering loop and reduces Phase 3's
scope to effect-path textures only.

**Tier 2** operates inside Phase B's render step. Phase 0 (logging removal)
is purely local and can proceed in parallel with all other work. Phases 1
and 3 are independent. Phase 2 builds on Phase 1. Phase 4 builds on
Phase 2. Phase 5 replaces Phases 2 and 4.

---

## Expected impact

### Tier 0 — Native view consolidation

| Phase | Impact | Metric | Status |
|-------|--------|--------|--------|
| NV-1 + NV-2 | Eliminates native view management overhead | 0 native child views to reposition on resize. No `SetWindowPos`, `setFrame:`, or `gtk_fixed_move` per child. Eliminates NSView (0,0) reset bug. Input events routed through virtual hit testing | **COMPLETED** |
| NV-3 | Reduces GPU resources from N to 1 | 1 render target, 1 GPU texture, 1 compositor surface per window instead of N. Eliminates N-1 render target rebuilds on resize. Implemented via `setViewportOverride()` and root visual surface sharing in `ensureLayerSurfaceTarget()` | **COMPLETED** |
| NV-4 | Eliminates coordinate conversion bugs | One coordinate system, one conversion point at root view | **COMPLETED** |

### Tier 1 — Scheduling architecture

| Phase | Impact | Metric |
|-------|--------|--------|
| Phase A | Eliminates queue flooding | Widgets deposit into single composite frame instead of queuing packets. Queue depth drops from O(widgets × resize_events) to 0 |
| Phase A-1 | Eliminates offscreen texture blit | Removes intermediate GPU texture, fullscreen-quad copy pass, and blit shader infrastructure. Saves 2–5ms per frame (the presentation blit). Eliminates per-visual texture allocation on resize |
| Phase B | Eliminates staggered partial updates | One render pass + one present per frame directly to native drawable. 10 widgets = 1 GPU submission, not 10 |
| Phase C | Eliminates resize content gaps | Single composite frame is always complete — no waiting for individual surfaces |

Tier 0 + Tier 1 together transform the resize scenario from 29–58
seconds of queue drain to immediate presentation at vsync rate,
regardless of widget count. The compositor goes from N surfaces × N
render targets × N presents to 1 surface × 1 render target × 1 present.

### Tier 2 — Per-command rendering

| Phase | Estimated per-frame savings | Cumulative | Status |
|-------|---------------------------|------------|--------|
| Phase 0 | 1–5ms (console I/O) | ~35–90ms | **COMPLETED** |
| Phase 1 | 10–25ms (submission overhead) | ~15–65ms | **COMPLETED** |
| Phase 2 | 10–25ms (tessellation) | ~5–40ms | Pending |
| Phase 3 | 0–5ms (effect-path resize only) | ~2–15ms | Pending (scope reduced by A-1) |
| Phase 4 | 0–15ms (skip unchanged) | ~2–10ms | Pending |

Note: Phase A-1 eliminates offscreen texture allocation for the common
no-effect path, reducing Phase 3's scope to effect-path textures only.
Phase 1's batched submission now covers ALL widgets' commands in the
window, not just one widget's.

After all tiers, a 30-command frame during resize should drop from
39–95ms to approximately 2–10ms — well within the 16.67ms budget for
60 FPS. Combined with one render pass and one present per frame, this
eliminates both the staggered rendering and the per-command overhead.

---

## Relationship to other plans

**Native View Architecture Plan:** Provides the full rationale, platform
research, and phased migration for the single-native-view-per-window
model. This plan incorporates the rendering-relevant aspects as Tier 0.
The Native View Architecture Plan also covers non-rendering concerns
(virtual hit testing, coordinate unification, `NativeViewHost` escape
hatch, accessibility) that are prerequisite to but not part of this plan.

**Batched Compositing Pass Plan:** Largely superseded by Tier 0 + Tier 1.
The Batched Compositing Pass Plan addressed compositing multiple layer
textures into the swapchain. With one render target per window (Tier 0),
there are no layer textures to composite — all widgets render directly
into the single surface, which presents to the swapchain. The entry-based
compositing concept is absorbed into Phase B's single render pass, where
widget slices are rendered in z-order with scissor rects.

**Frame Pacing Plan:** Provides backpressure to throttle production when
the compositor is overloaded. With the single-surface model, pacing
operates on the `CompositorSurface::deposit()` path — one surface, one
pacing signal. The `FramePacingMonitor` can feed into the frame loop's
trigger logic in Phase B, controlling how frequently the loop runs.
Together they provide both the efficiency (this plan) and the safety
valve (frame pacing) needed for smooth rendering.

**Stale Frame Coalescing Removal:** Fully superseded by this plan. The
coalescing mechanism (removed in Phase 1 of that plan) was a queue-level
optimisation for a queue that Tier 1 eliminates entirely. The remaining
phases (2–5 of the removal plan — deleting helper functions and telemetry)
become part of the Tier 1 implementation: when the command queue and
`CompositorCommand` hierarchy are replaced by the surface mailbox, all
coalescing infrastructure is removed as dead code.

---

## File change summary

### Tier 0 — Native view consolidation

#### NV-1 + NV-2 (COMPLETED)

| File | Phase | Changes |
|------|-------|---------|
| `wtk/include/omegaWTK/Native/NativeWindow.h` | NV-1 | **Done.** `addNativeItem()` removed from interface |
| `wtk/src/Widgets/BasicWidgets.cpp` | NV-1 | **Done.** `wireChild()` / `unwireChild()` use virtual tree operations |
| `wtk/include/omegaWTK/UI/View.h` | NV-2 | **Done.** Added `containsPoint()` for hit testing |
| `wtk/src/UI/View.Core.cpp` | NV-2 | **Done.** Implemented `containsPoint()` |
| `wtk/src/UI/WidgetTreeHost.h` | NV-2 | **Done.** Declared `hitTest()`, `hitTestWidget()`, `dispatchInputEvent()`, `hoveredView_` |
| `wtk/src/UI/WidgetTreeHost.cpp` | NV-2 | **Done.** Hit test (reverse z-order widget tree walk), event dispatch with hover tracking |
| `wtk/src/UI/AppWindow.cpp` | NV-2 | **Done.** `AppWindowDelegate::onRecieveEvent` routes input events to `WidgetTreeHost::dispatchInputEvent` |
| `wtk/src/UI/AppWindowImpl.h` | NV-2 | **Done.** Root NativeItem's `event_emitter` wired to AppWindow |

#### NV-3 through NV-5 (pending)

| File | Phase | Changes |
|------|-------|---------|
| `wtk/include/omegaWTK/UI/View.h` | NV-3 | Remove NativeItem dependency entirely (render target access refactored) |
| `wtk/src/UI/View.Core.cpp` | NV-3 | Remove `make_native_item()` from View constructor |
| `wtk/src/Native/macos/CocoaItem.mm` | NV-4 | Override `isFlipped` → YES on root view |
| `wtk/src/Native/win/HWNDItem.cpp` | NV-4 | Remove Y-inversion for children |
| `wtk/include/omegaWTK/UI/Widget.h` | NV-4 | Coordinate system uses top-left origin |
| New: `wtk/include/omegaWTK/UI/NativeViewHost.h` | NV-5 | `NativeViewHost` widget for embedding real native views |
| New: `wtk/src/UI/NativeViewHost.cpp` | NV-5 | Platform-specific native view embedding |

### Tier 1 — Scheduling architecture

| File | Phase | Changes |
|------|-------|---------|
| New: `wtk/include/omegaWTK/Composition/CompositorSurface.h` | A | `CompositorSurface` class (per-window): mailbox slot, `deposit()`, `consume()`, generation tracking. `CompositeFrame` struct with `WidgetSlice` entries |
| New: `wtk/src/Composition/CompositorSurface.cpp` | A | `CompositorSurface` implementation |
| `wtk/include/omegaWTK/Composition/CompositorClient.h` | A | `CompositorClientProxy` simplified: deposits widget commands into `CompositeFrame` builder instead of command queue |
| `wtk/src/Composition/Compositor.h` | A, B, C | Remove `CompositorCommand` hierarchy, `CompareCommands`, priority queue, sync lane tracking. Add per-window surface, frame loop, `ResizeState` |
| `wtk/src/Composition/Compositor.cpp` | A, B, C | Replace `CompositorScheduler` command loop with frame-oriented loop (consume → render → present). Remove `scheduleCommand`, `processCommand`, `dropQueuedStaleForLaneLocked`, `waitForLaneAdmission`, all packet lifecycle tracking. Single render pass for all widget slices |
| `wtk/src/UI/View.Core.cpp` | A, C | `endCompositionSession` appends to `CompositeFrame` builder. Resize path sets size generation |
| `wtk/src/UI/WidgetTreeHost.cpp` | A, C | Paint pass builds `CompositeFrame` and deposits into window surface. `notifyWindowResize()` increments generation |
| `wtk/src/Composition/backend/Execution.cpp` | A-1, B | Remove `presentAllPending()` calls. `executeCurrentCommand()` replaced by single-render-pass loop over `WidgetSlice` entries |
| `wtk/src/Composition/backend/RenderTarget.h` | A-1 | Remove `committedTexture`, `hasPendingContent`, `ViewPresentTarget`. Simplify `BackendCompRenderTarget` |
| `wtk/src/Composition/backend/RenderTarget.cpp` | A-1 | Pixel format → BGRA8Unorm. `beginFrame()`/`commit()` render directly to native drawable. Remove `blitAndPresent()`, `compositeAndPresentTarget()`, blit pipeline setup. Keep effect-path blit |
| `wtk/src/Composition/backend/mtl/CALayerTree.mm` | A-1 | Pass native target to `BackendRenderTargetContext` constructor |
| `wtk/src/Composition/backend/dx/DCVisualTree.cpp` | A-1 | Pass native target to `BackendRenderTargetContext` constructor |

### Tier 2 — Per-command rendering

| File | Phase | Changes |
|------|-------|---------|
| `wtk/src/Composition/backend/RenderTarget.cpp` | 0, 1, 2, 3, 4 | **0+1 DONE:** Logging gated behind `OMEGAWTK_TRACE_RENDER`, debug file writes removed. `beginFrame()`/`endFrame()` implemented. `renderToTarget()` records into open `frameCB_`. Pipeline state tracking. Texture fence end/restart. Remaining: tessellation cache integration, bucketed resize, frame hash comparison |
| `wtk/src/Composition/backend/RenderTarget.h` | 1, 2, 3, 4 | **1 DONE:** Added `beginFrame()`, `endFrame()`, `frameCB_`, `frameActive_`, `lastPipelineWasTexture_`. Remaining: `TessellationCache` member, deferred release queue, `lastRenderedHash` |
| `wtk/src/Composition/backend/Execution.cpp` | 0, 1, 4 | **0+1 DONE:** Logging gated, debug file writes removed. Command loop wrapped with `beginFrame()`/`endFrame()`. Clear dedup statics removed. Remaining: hash passthrough |
| `wtk/include/omegaWTK/Composition/Canvas.h` | 4 | `contentHash` on `CanvasFrame` |
| `wtk/src/Composition/Canvas.cpp` | 4 | Hash update in `draw*` methods |
| New: `wtk/src/Composition/backend/TessellationCache.h` | 2 | `ShapeKey`, `CachedTessellation`, `TessellationCache` |
| `wtk/src/Composition/backend/BufferPool.h` | 1 | Support for pinned (non-recyclable) buffers |
| `wtk/src/Composition/backend/TexturePool.h` | 3 | Bucket-aware acquire |

---

## References

### Codebase

#### Tier 0 — Native view consolidation
- **NV-1 (COMPLETED):** `addNativeItem()` removed from `NativeWindow` interface. `wireChild()` uses virtual tree. `WindowLayer` deleted. Views still create NativeItems for render target access (removed in NV-3)
- **NV-2 (COMPLETED):** `WidgetTreeHost::hitTest()` — reverse z-order widget tree walk. `dispatchInputEvent()` — extracts position, hit tests, dispatches to target View. Hover tracking synthesizes `CursorEnter`/`CursorExit`. `AppWindowDelegate` routes input events. Root NativeItem `event_emitter` wired to AppWindow
- Native view creation: `View.Core.cpp:77` (`make_native_item` — still present, removed in NV-3)
- Hit test entry: `WidgetTreeHost.cpp` (`hitTest`, `hitTestWidget`, `dispatchInputEvent`)
- Event routing: `AppWindow.cpp` (input cases in `AppWindowDelegate::onRecieveEvent`)
- Root emitter wiring: `AppWindowImpl.h` (`nativeWindow->getRootView()->event_emitter = &owner`)

#### Tier 1 — Scheduling architecture (to be replaced)
- Compositor scheduler loop: `Compositor.cpp:395–455` (`CompositorScheduler` constructor, serial pop → process → present)
- `scheduleCommand()`: `Compositor.cpp:530–545` (packet enqueue + coalescing call site)
- `processCommand()`: `Compositor.cpp:460–525` (single-command dispatch)
- `waitForLaneAdmission()`: `Compositor.cpp:636–661` (per-lane in-flight gate)
- `CompositorCommand` hierarchy: `CompositorClient.h:45–120` (command types, priority)
- `CompareCommands`: `Compositor.h:105–130` (7-level priority comparator)
- `CompositorClientProxy::submit()`: `CompositorClient.h:160–185` (command packaging + scheduling)
- `presentAllPending()`: `RenderTargetStore` call in scheduler loop (per-iteration present)
- Widget submission path: `Widget.Paint.cpp:35–70` (`executePaint` → composition session → submit)
- View composition session: `View.Core.cpp:135–151` (`startCompositionSession` / `endCompositionSession`)
- Widget tree resize: `WidgetTreeHost.cpp:85–110` (`notifyWindowResize` → recursive invalidation)

#### Phase A-1 — Direct-to-drawable rendering (blit removal)
- Offscreen texture allocation: `RenderTarget.cpp:486–509` (`BackendRenderTargetContext` constructor, RGBA8Unorm)
- Texture format key: `RenderTarget.cpp:511–578` (`rebuildBackingTarget`, `RGBA8Unorm` pool key)
- Pipeline format default: `GEPipeline.h:52` (`colorPixelFormat = PixelFormat::RGBA8Unorm` — must change to BGRA8Unorm)
- Native drawable format: `GEMetalRenderTarget.h` (`pixelFormat()` returns `BGRA8Unorm`)
- `beginFrame()`: `RenderTarget.cpp:703` (currently targets offscreen texture)
- `commit()`: `RenderTarget.cpp:762` (sets `committedTexture`, `hasPendingContent`)
- `blitAndPresent()`: `RenderTarget.cpp:1454` (fullscreen-quad shader blit — removed by A-1)
- `compositeAndPresentTarget()`: `RenderTarget.cpp:1498` (fast/general path blit — removed by A-1)
- `getFinalCopyPipelineForFormat()`: `RenderTarget.cpp:402` (format-specific copy pipelines — removed by A-1)
- `presentAllPending()`: `RenderTarget.cpp:1635` (iterates store — simplified by A-1)
- Pipeline creation: `RenderTarget.cpp:226–265` (render pipeline states, format must match drawable)
- Visual context creation: `mtl/CALayerTree.mm:137` (passes `nullNative` — A-1 passes real target)

#### Tier 2 — Per-command rendering
- Per-command render pass: `RenderTarget.cpp:1266–1297`
- Per-command tessellation: `RenderTarget.cpp:830–1078` (`triangulateSync` calls at 847, 874, 894, 987, 1057, 1062)
- Ellipse CPU triangle fan: `RenderTarget.cpp:929–953`
- Per-vertex CPU transform: `RenderTarget.cpp:1169–1173`
- Render target rebuild: `RenderTarget.cpp:503–578` (`rebuildBackingTarget`)
- Resize trigger: `RenderTarget.cpp:620–635` (`setRenderTargetSize`)
- GPU sync wait: `RenderTarget.cpp:527–528` (`waitForGPU` in rebuild)
- Commit sync wait: `RenderTarget.cpp:711` (`waitForGPU` in commit)
- Console logging: `RenderTarget.cpp:799–804, 1266, 1279, 1293, 1296, 1301`
- Debug file logging: `RenderTarget.cpp:1131–1132`
- Existing texture pool: `TexturePool.h:79–135`
- Existing buffer pool: `BufferPool.h:78–121`

#### Related plans
- Native view architecture: `wtk/docs/Native-View-Architecture-Plan.md`
- Compositing plan: `wtk/docs/Batched-Compositing-Pass-Plan.md`
- Frame pacing plan: `wtk/docs/Frame-Pacing-Plan.md`
- Stale frame coalescing removal: `wtk/docs/Stale-Frame-Coalescing-Removal-Plan.md`

### External

#### Chromium — native view architecture
- Chromium creates one native view per top-level window. All browser chrome is a virtual `views::View` tree painted onto that single surface using Skia
- macOS: 1 `BridgedContentView` (NSView) per window + 1 `RenderWidgetHostViewCocoa` per visible tab
- Windows: 1 HWND per window + 1 legacy `Chrome_RenderWidgetHostHWND` for accessibility
- Linux: 1 X11/Wayland surface per window. No child native views
- `views::View` has no native platform backing. Hit testing via `ViewTargeter`, painting via `OnPaint()` into shared canvas
- `NativeViewHost` (`ui/views/controls/native/native_view_host.h`): escape hatch for embedding real native views (IME, web content)
- Aura (`ui/aura/window.h`): lightweight window tree within single native window on Windows/Linux/ChromeOS

#### Other toolkit native view models
- Apple: ~100 NSViews practical limit. Recommends single-view-manages-lightweight-objects for hundreds of elements
- WPF: single HWND per window, milcore DirectX composition, all controls windowless
- WinUI 3: single HWND, all controls windowless, no `HwndHost` equivalent
- Qt 4.4+ "alien widgets": only top-level QWidgets get native windows. "Significantly speeds up widget painting, resizing, and removes flicker"
- GTK4: one `GdkSurface` per toplevel. GSK scene graph. "Child widgets don't have their own surface"
- Flutter: single native surface per window, all rendering via Skia/Impeller

#### Chromium viz — display compositor architecture
- `viz::Surface` / `SurfaceManager`: per-producer mailbox slot holding latest `CompositorFrame`. Producers submit independently; display compositor reads all surfaces during aggregation
- `SurfaceAggregator::Aggregate()`: reads latest frame from every registered surface in one pass, merges into single `AggregatedFrame`, resolves surface references and quads
- `DisplayScheduler`: vsync-driven loop. Sends `BeginFrame` to all clients, waits for responses (or deadline), calls `Display::DrawAndSwap()` for single present
- `LocalSurfaceId`: `(parent_sequence_number, child_sequence_number)` — generation tracking for resize coordination. Compositor compares surface IDs to determine which surfaces have "caught up" to the current size
- `BeginFrame` broadcast: display compositor sends `BeginFrameArgs` (frame time, deadline, interval) to all producers. Producers submit before the deadline or forfeit the frame

#### Chromium — rendering and GPU
- `gpu::CommandBuffer` / `gpu::CommandBufferHelper`: command serialization into shared memory, single `Flush()` IPC per batch
- `PaintOpBuffer`: flat array of serialized Skia operations, replayed once against a GPU surface
- `SharedImagePool`: render target texture pooling by (size, format), eliminates per-frame allocation
- `TileManager` + `DisplayItemList`: per-tile dirty tracking, only re-rasterizes tiles whose display items changed
- `SkiaRenderer`: one `GrDirectContext::flush()` per composited frame = one GPU submission

#### GPU APIs
- Vulkan `VkCommandBuffer`: all draw calls recorded into one buffer, one `vkQueueSubmit` per frame
- Metal `MTLRenderCommandEncoder`: all draws encoded into one encoder per render pass, one `[cb commit]` per frame

#### Game engines
- Game engine frame loop: sequential phases (update → gather → render → present), single present per frame, no individual source can trigger presentation
- Unreal RDG (Render Dependency Graph): transient resource allocation from `FRHITransientResourceHeap` with memory aliasing across non-overlapping passes
- Unity static mesh batching: multiple meshes with same material combined into single vertex buffer at build time
- Game engine ring buffers: single large pre-allocated vertex buffer divided into per-frame segments, no allocation per draw
- SDF rendering for UI: Mapbox GL (text/icons), Unreal Slate, Unity UGUI — quad + fragment shader SDF evaluation, zero tessellation
