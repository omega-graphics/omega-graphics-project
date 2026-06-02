# NativeWindow Ready Signal — Plan

**Scope:** Add a cross-platform, per-window "the native surface is realized and the engine may render into it" signal, and gate the WTK render dispatch on it. Replaces ad-hoc deferred root-visual creation that we would otherwise be tempted to bolt onto the Vulkan/Linux path only.

**Out of scope:**
- *Subsequent* re-realize cycles (DPI scale change, Wayland scale/transform update, Windows display-change, etc.). The hook this plan adds is what Phase F of `UIView-Render-Redesign-Plan.md` routes through; the full-tree-repaint-on-re-realize behavior itself lives in Phase F.
- The X11 child-surface path. `X11SurfaceHost::runOnRealize` (per `Native-API-Completion-Proposal.md` §2.13) keeps its local realize gate for child Windows — this plan owns only the *root* native surface gate. The two compose naturally: a child surface can't realize before its toplevel does.
- Wayland support. §2.13 commits to X11-only for the GTK backend; this plan inherits that scope. Wayland's `xdg_surface.configure` ack semantics are noted below as a future-work seam, not implemented.

---

## 1. Problem statement

`SVGViewRenderTest` renders black on Linux/Vulkan. The trace (after the `[WTK_RP]` instrumentation in `wtk/src/UI/FrameBuilder.cpp` and `wtk/src/Composition/Compositor.cpp`) shows:

1. FrameBuilder runs the first paint, produces `slices=1`, deposits to the surface, wakes the worker.
2. `Compositor::renderCompositeFrame` enters with `slices=1`, then silently exits inside the `!hasRootVisual()` block — the slice has no `targetLayer`, so the anchor-layer search returns null and the function returns without creating the root visual or dispatching to GTE.
3. Four `ResizeSession` cycles fire (Active + 3 Settling + Completed) as the WM negotiates the toplevel size. Each cycle drives a FrameBuilder pass, but the paint walk's `DirtyBits` gate emits `slices=0` because nothing in the view tree has been marked dirty.
4. After the resize handshake, no further dirty signal arrives. The swapchain image is whatever the WM committed during the resize — i.e., the toplevel's default content. The SVG never reaches GTE.

This is two coupled defects in one symptom:

- **Defect A — the first paint runs too early.** It executes before the native surface is realized, before any `BackendCompRenderTarget` has been populated with a usable root visual, before GTE has acquired a swapchain image. The deposited frame is correct content but lands in a render path that cannot use it yet.
- **Defect B — there is no recovery paint.** Resize-driven repaints carry no dirty bits, so they emit no slices. Even if Defect A were fixed by chance, the dirty-bit machinery wouldn't re-emit the SVG until the view tree itself was dirtied.

Defect A is what this plan addresses. Defect B is Phase F's territory.

### Why this only manifests on Linux/Vulkan *today* — and won't stay that way

- **macOS (Metal), today:** `NSWindow`'s `CAMetalLayer` is live before `applicationDidFinishLaunching:` returns, *and* `PaintOptions::autoWarmupOnInitialPaint` (see `Widget-View-Paint-Lifecycle-Plan.md` line 1422-1432) submits the initial frame multiple times. Both together mask any race between window construction and a fully on-screen surface. The race exists; it's just not visible.
- **Windows (D3D12), today:** `WM_SHOWWINDOW` + first `WM_PAINT` fire before the app's render thread services its first frame for the same loop-ordering reason, plus warmup. Same masking.
- **Linux/X11 (Vulkan via GTK), today:** the X11 `Window` does not exist until GTK fires `realize` on the toplevel — and that gap is *longer* than warmup can hide, so the bug surfaces. This is exactly the gap §2.13 calls out for the X11 surface ownership path.
- **All three, after `Widget-View-Paint-Lifecycle-Plan.md` Tier D lands:** warmup goes away ("`PaintOptions::autoWarmupOnInitialPaint` and `warmupFrameCount` are deprecated. The first frame is submitted once, like every other frame." — line 893-895). The one initial frame must land *after* the surface is ready, on every backend, or it gets dropped. There is no longer a buffer to hide the race.

The fix therefore has to be cross-platform from the start. A Linux-only override would unblock SVGViewRenderTest today but would re-break macOS and Windows the moment Tier D removes warmup. See `feedback_clean_uniform_fixes` and `feedback_frontend_backend_uniformity` in the project memory.

---

## 2. Goals & non-goals

**Goals:**
- One cross-platform per-window signal: "the native surface is realized; render dispatch is safe."
- Frames produced *before* the signal fires are *deferred* (not dropped) and replay automatically once it does.
- The mechanism collapses any temptation to add platform-specific deferred-root-visual creation in `Compositor.cpp`.

**Non-goals:**
- Re-realize handling → Phase F of `UIView-Render-Redesign-Plan.md`.
- Child-surface realize → §2.13's `X11SurfaceHost::runOnRealize`.
- Coalescing or rate-limiting pre-ready paints. Pre-ready paints can replace each other in the surface's pending slot the same way the steady-state most-recent-wins behavior already does; we don't need a queue.
- Wayland. Note the seam, don't implement.

---

## 3. Design

### 3.1 `NativeWindow` API addition

Add three virtual methods to `INTERFACE NativeWindow` in `wtk/include/omegaWTK/Native/NativeWindow.h`. Defaults are deliberately "realized-once-and-forever" so every existing backend behaves exactly as today until it opts in:

```cpp
/// Whether the native surface is realized and the engine may render
/// into it. Backends with a meaningful gap between window construction
/// and surface availability (GTK/X11) override; backends where the
/// surface is live before the first compositor wake (macOS, Windows)
/// return true unconditionally. May transiently return false during a
/// re-realize cycle (DPI change, Wayland scale change, Windows
/// display-change recreation) and then return true again; subscribers
/// to `onRealize` are how that transition is observed.
virtual bool isNativeReady() const { return true; }

/// Register a one-shot callback that fires exactly once: when the
/// native surface is realized for the first time. If `isNativeReady()`
/// already returns true at registration time, fires synchronously on
/// the calling thread (replays the past first-realize event for
/// late subscribers). Otherwise fires on the platform's UI thread when
/// the initial realize signal arrives. Never fires again — subsequent
/// re-realize transitions are delivered via `onRealize`. Registration
/// is permanent for the NativeWindow's lifetime; there is no unregister.
///
/// This plan uses `onFirstRealize` to release the deferred initial
/// paint queued in the window's `CompositorSurface`.
virtual void onFirstRealize(std::function<void()> cb) { if(cb) cb(); }

/// Register a sticky (refireable) callback that fires on every
/// *subsequent* re-realize transition — explicitly NOT on the first
/// realize. The first realize is delivered through `onFirstRealize`;
/// this method covers DPI scale change, Wayland scale/transform
/// update, Windows display-change recreation, macOS layer re-host on
/// space switch. Callbacks fire on the platform's UI thread in
/// registration order. Registration is permanent for the
/// NativeWindow's lifetime — there is no unregister; callbacks must
/// own their own teardown guard (e.g. capture a weak_ptr to whatever
/// state they touch).
///
/// Default impl never fires: most windows on most platforms have a
/// single realize cycle. Backends that genuinely re-realize override.
///
/// Phase F of UIView-Render-Redesign-Plan.md uses `onRealize` to force
/// a full-tree repaint on every re-realize. Splitting first-realize
/// (`onFirstRealize`) from subsequent-realize (`onRealize`) means
/// Phase F's full-repaint walker doesn't redundantly run at startup
/// alongside the deferred-paint release this plan owns.
virtual void onRealize(std::function<void()> cb) { (void)cb; }
```

The two event subscriptions are **non-overlapping**: every realize transition is delivered through exactly one of them (`onFirstRealize` for the singular initial event, `onRealize` for the stream of subsequent ones). Subscribers pick the one matching the event they care about; no filtering or first-fire guards are needed in callback code.

Naming choices:
- `isNativeReady` for the boolean state-query (reads as "is the native item ready *right now*?"). Avoids `isRealized` (X11-specific, and transient-false-during-re-realize would make it a confusing predicate) and `isShown` (collides with `isVisible()`'s map/unmap semantics).
- `onFirstRealize` for the singular event, `onRealize` for the stream — the `First` qualifier reads naturally as the singleton, and the unqualified `onRealize` reads as the steady-state event-bus subscription, matching `onResize`-style conventions elsewhere in the codebase.

### 3.2 Per-platform realize triggers

All three currently-supported backends must override. Defaults exist for symbol availability and forward-compat with future backends, not as a substitute for a real override on any backend that ships today.

| Backend | File | First-realize trigger | Re-realize trigger (`onRealize`) |
|---|---|---|---|
| macOS (`Cocoa`) | `wtk/src/Native/macos/CocoaAppWindow.mm` | First `windowDidUpdate:` notification after `NSWindow` is visible *and* `CAMetalLayer` has a valid drawable. Equivalent: first successful `nextDrawable` on the layer. Either is observable from the NSWindowDelegate the AppWindow already owns. | `windowDidChangeBackingProperties:` (display change / scale factor change) and `viewDidMoveToWindow:` on the host NSView (layer re-host on space switch / fullscreen toggle). |
| Windows (`Win32`) | `wtk/src/Native/win/WinAppWindow.cpp` | First `WM_PAINT` *or* `WM_SHOWWINDOW(TRUE)` whose `BeginPaint`/swap-chain query confirms the swap chain is presentable. Flip the atomic in the wndproc. | `WM_DPICHANGED` (DPI scale change), `WM_DISPLAYCHANGE` (display reconfiguration) — either may invalidate the swap chain and force a recreate. |
| Linux X11 (`GTK`) | `wtk/src/Native/gtk/GTKAppWindow.cpp` | GTK `realize` signal on the toplevel `GtkWindow` (per §2.13's `onWindowRealize`). | `configure-event` carrying a scale change, `notify::scale-factor`, or any forced surface recreate path §2.13's `X11SurfaceHost` introduces. |
| Linux Wayland | (future) | First `xdg_surface.configure` ack + `wl_surface.commit` round trip. | Subsequent `xdg_surface.configure` with scale or transform changes; `wl_output` scale change. |

The first-realize trigger fires exactly once per `NativeWindow` lifetime. Re-realize cycles fire `onRealize` — Phase F consumes that, not this plan.

For each backend the "trigger" above is the *latest-arriving* signal that must be observed before `isNativeReady()` flips to `true`. Earlier signals are fine to track too, but `isNativeReady()` must not return `true` until every condition for "a frame submitted right now will land on the visible surface" is satisfied — that's what's load-bearing once warmup is gone.

### 3.3 Compositor render-deferral mechanism

Choice of gate point (three candidates):

1. **`FrameBuilder::endFrame` refuses to `deposit` pre-ready.** Bad: throws away correct content; recovery requires a re-paint trigger from somewhere.
2. **`Compositor::renderCompositeFrame` early-returns pre-ready.** Bad: `surface->consume()` has already drained the pending frame, so the content is lost the same way.
3. **`Compositor::drainWindowSurfaces` skips `consume()` for not-ready surfaces.** ✓ Frame stays in the pending slot, surface stays in `hasPendingUpdate=true` state, next drain wake re-tries. No content loss, no extra storage, no replay queue.

Sketch — minimal change at `wtk/src/Composition/Compositor.cpp:158`:

```cpp
void Compositor::drainWindowSurfaces(){
    // …existing snapshot capture…
    for(auto & entry : snapshot){
        auto & surface = entry.second;
        if(surface == nullptr || !surface->hasPendingUpdate()){
            continue;
        }
        // NEW: ask the AppWindow whether its native surface is realized.
        auto * appWindow = surface->ownerAppWindow();          // see §3.5
        if(appWindow != nullptr &&
           !appWindow->nativeWindow()->isNativeReady()){
            // Leave the frame in the surface; the onNativeReady callback
            // will re-wake the worker and we'll see this surface again.
            continue;
        }
        auto frame = surface->consume();
        if(frame == nullptr){ continue; }
        renderCompositeFrame(entry.first, frame);
    }
}
```

The `onFirstRealize` callback registered once per AppWindow is:

```cpp
// In AppWindow ctor, after windowSurface/registerWindowSurface wiring:
impl_->native->onFirstRealize([weakComp = std::weak_ptr<Compositor>(comp)]{
    if(auto comp = weakComp.lock()){
        comp->scheduleFrameWake();   // sets frameDirty_ + notifies
    }
});
```

`scheduleFrameWake` is the existing flip-frameDirty_-and-notify pattern that `surface->deposit()` already uses; expose it as a named member if it isn't already.

Phase F's re-realize → full-repaint hook is a *separate* registration against `onRealize` (the subsequent-events stream). The two subscriptions are independent: this plan's `onFirstRealize` fires once at startup to release the deferred initial paint, Phase F's `onRealize` fires on every re-realize to force a full-tree repaint. Neither subscriber needs to know about the other, and neither runs redundantly — the API split is what guarantees that.

### 3.4 Thread-safety

- `isNativeReady()` is queried from the `CompositorFrameWorker` thread. Implementations should back it with `std::atomic<bool>` — the realize trigger writes (true on realize, transiently false during re-realize), the drain-loop reads many times.
- `onFirstRealize` / `onRealize` registration may happen from any thread; callbacks may be invoked from the UI thread (per realize transition) or the calling thread (synchronous fast-path fire when `onFirstRealize` is called after the first realize already happened). Receivers (the Compositor lambda above) must therefore be safe to run from either — `scheduleFrameWake` already is.
- Re-entrancy: if `onFirstRealize`'s callback fires synchronously inside the registration call, we must not deadlock. The Compositor lambda only sets a flag and notifies; it does not take any lock that the caller might hold. Document this as a constraint on both `onFirstRealize` and `onRealize` callbacks ("must not take locks held by the caller").
- Two-list storage in the override: the GTK override holds two `std::vector<std::function<void()>>` members — one for `onFirstRealize` subscribers, one for `onRealize`. The realize handler fires the first-realize list once (then clears it to release any captured state) and the re-realize list on every subsequent transition. Two registrations against the same `NativeWindow` (e.g., this plan's first-paint wake + Phase F's full-repaint trigger) land in different lists and are dispatched on disjoint events; neither is allowed to throw or to take a lock that blocks the other.

### 3.5 The `surface → AppWindow` back-edge

The Compositor today only knows about `CompositionRenderTarget` and `CompositorSurface`. To consult `nativeWindow()->isNativeReady()` it needs a path back to the `AppWindow`.

Two options:

**A. Store an `AppWindow*` on `CompositorSurface` at `registerWindowSurface` time.** Smallest change. The Compositor's `windowSurfaces_` map already pairs the target with the surface; we add the back-pointer. Lifetime: `AppWindow` outlives its `CompositorSurface` by construction, so a raw pointer is safe.

**B. Pass `std::function<bool()> isReady` and `std::function<void(std::function<void()>)> onReady` into `registerWindowSurface`.** Decouples Compositor from `AppWindow` / `NativeWindow`. Slightly more plumbing, no header coupling.

Recommendation: **A** for now (one pointer, no lambdas), revisit if the Compositor needs to stay headless from WTK UI for testing.

---

## 4. Migration / sequencing

Four landings, each independently shippable:

1. **API + no-op defaults.** [DONE] Add `isNativeReady` / `onFirstRealize` / `onRealize` to `NativeWindow.h` with the default impls in §3.1. No behavior change anywhere; just makes the symbols available. Zero risk.

2. **GTK override.** [DONE] `GTKAppWindow` overrides all three, backed by an `std::atomic<bool>` flipped in the existing `realize` handler (or the new one §2.13 adds). Two `std::vector<std::function<void()>>` members hold the pending subscribers: one for first-realize (drained and cleared after the first fire), one for re-realize (sticky, fires on every subsequent transition in registration order). Linux behavior change but no other path exercises it.

3. **Compositor gate + back-edge.** [DONE] Add the `AppWindow*` back-edge per §3.5(A), wire the `drainWindowSurfaces` skip per §3.3, register the `onFirstRealize` wake on `AppWindow` construction. SVGViewRenderTest now renders the SVG on Linux. macOS/Windows behavior is unchanged at this step because their `isNativeReady` still returns true unconditionally via the defaults.

4. **macOS override (required-before-Tier-D).** `CocoaAppWindow.mm` overrides `isNativeReady` (atomic flag flipped on first valid drawable / first `windowDidUpdate:` after visible) and `onFirstRealize` / `onRealize` with the same two-vector storage pattern as GTK. Plus the re-realize triggers from §3.2.

5. **Windows override (required-before-Tier-D).** `WinAppWindow.cpp` overrides the same trio, atomic flipped in the wndproc on the first `WM_PAINT`/`WM_SHOWWINDOW` that confirms the swap chain is presentable. Plus `WM_DPICHANGED` / `WM_DISPLAYCHANGE` as re-realize triggers.

6. **(Future) Wayland override.** When the Wayland backend lands per §2.13's followup workstream, it overrides too — first `xdg_surface.configure` ack + commit for first realize, subsequent configures with scale/transform changes for re-realize.

Phase F's re-realize hook lands separately as a second registration against `onRealize` (not `onFirstRealize`). Because the two subscriptions are non-overlapping by design — `onFirstRealize` fires once at startup, `onRealize` fires only on subsequent re-realizations — Phase F's full-tree-repaint walker never runs redundantly alongside this plan's first-paint release. No spec change is needed between this plan and Phase F; the split-API choice is what guarantees the non-redundancy.

**Hard temporal ordering with `Widget-View-Paint-Lifecycle-Plan.md` Tier D:** steps 4 and 5 above must land *before* Tier D removes warmup (Tier D's Block 2 step: "Retire `PaintOptions::{autoWarmupOnInitialPaint, warmupFrameCount, …}`" — `Widget-View-Paint-Lifecycle-Plan.md` line 1061). Today, warmup masks the same race on macOS/Windows that Linux exhibits; once warmup is gone the one initial frame either lands after the surface is ready or it gets dropped. The `isNativeReady` gate is what makes "lands after" the guaranteed outcome. Land the per-backend overrides first, verify each on a real `SVGViewRenderTest`-equivalent, *then* let Tier D pull warmup. Reversing the order is a regression on every platform.

---

## 5. Validators

- `wtk/tests/SVGViewRenderTest` renders the SVG content on Linux/X11 after the first paint, without resize-triggered repaints in the path.
- The `[WTK_RP] renderCompositeFrame` trace shows `reached dispatch, slices=N` instead of an early-return in the `!hasRootVisual()` block.
- The `[GEVulkan_RP] startRenderPass` trace fires for the first frame, with the SVG's slice count and a non-degenerate viewport.
- macOS run of any existing WTK test produces a byte-identical render path before and after (since `isNativeReady` defaults to true on macOS). No regression in `OmegaTimeAnimationTest` or similar.
- A multi-window stress (two `AppWindow`s, one realized via `initialDisplay`, the other deferred) renders each correctly and independently — first window's paint isn't blocked by the second's pre-realize state.

---

## 6. Risks / open questions

1. **One-shot vs re-fireable.** ~~Decision deferred.~~ **Resolved (2026-06-02):** split into two non-overlapping subscriptions — `onFirstRealize` (one-shot, fires once ever at the initial realize) and `onRealize` (sticky, fires only on subsequent re-realize transitions, never on the first). This plan registers `onFirstRealize` to release the deferred first paint; Phase F registers `onRealize` to force a full-tree repaint on every re-realize. Because the two events are disjoint by API contract, neither subscriber runs redundantly at startup. The misuse risk (forgotten registrations leaking callbacks) is mitigated by the lifetime contract: registration is permanent for the `NativeWindow`'s lifetime, callbacks must capture weak references to any state they touch, and there is no unregister. Document the contract on the interface so future subscribers don't bake an "unregister later" assumption into their lambdas.

2. **Wayland.** `xdg_surface.configure` is a continuing handshake (the client must `ack_configure` before committing). The "ready" moment is the first ack-then-commit round trip, not the first configure. Documented as a future seam — when Wayland support arrives, its `isNativeReady` flips after the first commit ack, not on `wl_surface` creation. No code in this plan blocks that future.

3. **Pre-realize `setRect` / state mutation.** If the app calls `setRect`, `setMenu`, `setTitle` before realize, those should either queue or take effect immediately on the GtkWindow (GTK handles most of these pre-realize). Not this plan's problem, but worth a test that pre-realize mutations land correctly after realize.

4. **`registerWindowSurface` ordering.** Today `AppWindow` calls `registerWindowSurface` synchronously in its ctor. After this plan, `registerWindowSurface` still runs eagerly; only the *render dispatch* on that surface defers. So the Compositor's `windowSurfaces_` map and the `frameDirty_` plumbing still mirror today's behavior — the only new gate is at `drainWindowSurfaces`'s consume step.

5. **Already-realized fast path race.** If `onNativeReady` is called from thread A while thread B (UI thread) is mid-flipping the realize flag, we could miss the fire. The simplest fix: take the same mutex the GTK override uses around both the flag flip + pending-callback drain and the registration path. Standard double-checked-locking pattern; document it on the override, not on the interface.

6. **Defect B is still present.** Even with this plan landed, a resize that produces `slices=0` won't show new content — the resize-driven repaint problem is unchanged. The first-paint case is fixed; resize correctness lives in Phase F.

7. **Temporal coupling to `Widget-View-Paint-Lifecycle-Plan.md` Tier D.** This plan must land its macOS and Windows overrides *before* Tier D's warmup removal (`PaintOptions::autoWarmupOnInitialPaint`/`warmupFrameCount`). Reverse the order and the macOS/Windows first paint races the same surface-not-ready failure Linux exhibits today, just without anyone having seen it once because warmup hid it. The validators in §5 should be exercised on macOS and Windows specifically with a build that pins `autoWarmupOnInitialPaint = false` *before* Tier D itself lands — a forward simulation of the post-warmup world — to confirm the gate behaves correctly without the warmup buffer in front of it. Track this in the implementation checklist: "macOS override done + verified", "Windows override done + verified", then "warmup removal cleared for landing."

---

## 7. Per-file change summary

- `wtk/include/omegaWTK/Native/NativeWindow.h` — add `isNativeReady()` + `onFirstRealize(cb)` + `onRealize(cb)` virtuals with default impls. One header, ~22 lines including doc comments.
- `wtk/src/Native/gtk/GTKAppWindow.h/.cpp` — override all three. Realize handler flips the atomic, drains and clears the first-realize list on the initial fire, and fires the (sticky) re-realize list on every subsequent transition. ~40 lines + the realize-signal wiring (§2.13 may have already added the signal handler; reuse if so).
- `wtk/src/Native/macos/CocoaAppWindow.h/.mm` — override all three. NSWindowDelegate observes `windowDidUpdate:` (first-realize) and `windowDidChangeBackingProperties:` (re-realize); the host NSView observes `viewDidMoveToWindow:` for layer re-host on space/fullscreen transitions. Same two-vector storage pattern as the GTK override. ~50 lines.
- `wtk/src/Native/win/WinAppWindow.h/.cpp` — override all three. Wndproc flips the atomic on first `WM_PAINT`/`WM_SHOWWINDOW` confirming swap-chain presentability, and dispatches `onRealize` on `WM_DPICHANGED` / `WM_DISPLAYCHANGE`. ~50 lines.
- `wtk/include/omegaWTK/Composition/CompositorSurface.h` (or wherever the surface lives) — add `AppWindow* ownerAppWindow_` field + accessor.
- `wtk/src/Composition/Compositor.cpp` — `drainWindowSurfaces` consults `surface->ownerAppWindow()->nativeWindow()->isNativeReady()` before `consume`. ~6 new lines.
- `wtk/src/UI/AppWindow.cpp` — at the end of the ctor (after `registerWindowSurface`), call `nativeWindow()->onFirstRealize([compWeak]{ comp->scheduleFrameWake(); })`. ~5 lines.
- `wtk/include/omegaWTK/Composition/Compositor.h` — expose `scheduleFrameWake()` if it isn't already (today the flip-and-notify happens inside `CompositorSurface::deposit`; this plan calls it from a second site).

Total diff is closer to ~180 lines across three native backends than the original ~80-line single-backend estimate. Still one PR, just bigger.

Estimated diff: ~80 lines including the GTK realize-handler wiring, single PR.

---

## 8. Dependencies

- **This plan depends on:** Nothing currently blocking. §2.13's `onWindowRealize` signal handler is convenient to share but not required; if §2.13 lands first the GTK override piggybacks on its handler, otherwise this plan adds it.
- **This plan enables:**
  - **`Widget-View-Paint-Lifecycle-Plan.md` Tier D's warmup removal** — once macOS/Windows overrides ship and validate, Tier D can pull `PaintOptions::autoWarmupOnInitialPaint` without regressing first-frame correctness on either backend. This is the *hard* downstream dependency (see Risk #7).
  - **Phase F of `UIView-Render-Redesign-Plan.md`** — re-realize → full-tree repaint hooks into `onRealize` (the subsequent-events subscription).
  - **A clean removal** of any future temptation to add deferred root-visual creation in `Compositor::renderCompositeFrame` on a per-backend basis.

The interim diagnostic logging in `Compositor.cpp` and `FrameBuilder.cpp` (the `[WTK_RP]` lines added during the bug investigation) can stay as-is — they're gated on `OmegaGTE::isDebugLayerEnabled()` and remain useful for verifying the validator in §5. Decide on removal as a follow-up; not a blocker for this plan.
